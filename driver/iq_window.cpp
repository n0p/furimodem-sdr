#include "iq_window.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fmiq {
namespace {

constexpr unsigned long kIocWindowLen = 0x80044331UL;  // memory window length

// Offsets of the common header (little-endian u32). The first five are the ones of the
// previous format; the rest only exist in "CSD1".
constexpr std::size_t kOffSig = 0x00;
constexpr std::size_t kOffSeq = 0x04;
constexpr std::size_t kOffUcnt = 0x08;
constexpr std::size_t kOffBytes = 0x0C;
constexpr std::size_t kOffState = 0x10;
constexpr std::size_t kOffBlk = 0x14;
constexpr std::size_t kOffPush = 0x18;
constexpr std::size_t kOffSlot = 0x1C;
constexpr std::size_t kOffP2 = 0x64;
constexpr std::size_t kOffP3 = 0x68;
constexpr std::size_t kOffStale = 0x6C;

std::uint32_t get_u32(const std::uint8_t* p, std::size_t off) {
    // The CCCI window is Device memory: it only accepts aligned 32-bit accesses. The
    // compiler can merge a 4 B memcpy with the neighbouring one into an 8/16 B access
    // (or an unaligned one), and the bus returns SIGBUS/BUS_ADRALN in the middle of the
    // header. The volatile 32-bit read prevents that merging.
    const volatile std::uint32_t* w = reinterpret_cast<const volatile std::uint32_t*>(p + off);
    return *w;
}

std::string errno_msg(const char* what, const std::string& path) {
    return std::string(what) + " (" + path + "): " + std::strerror(errno);
}

// Copies from the Device window in aligned 32-bit words. A large memcpy would be
// vectorized (LDP/NEON) and the CCCI bus answers SIGBUS/BUS_ADRALN.
void copy_from_window(std::uint8_t* dst, const std::uint8_t* src, std::size_t n) {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const std::uint32_t v = get_u32(src, i);
        std::memcpy(dst + i, &v, 4);
    }
    for (; i < n; ++i) {
        // Remainder not a multiple of 4: read the aligned word that contains it, never a
        // lone byte (Device memory does not accept unaligned 8-bit accesses either).
        const std::uint32_t v = get_u32(src, i & ~std::size_t(3));
        dst[i] = static_cast<std::uint8_t>((v >> (8 * (i & 3))) & 0xFFu);
    }
}

}  // namespace

IqWindow::IqWindow(IqRingLayout layout) : layout_(layout) {}

IqWindow::~IqWindow() { close(); }

bool IqWindow::open(const std::string& node, std::string* err) {
    close();
    fd_ = ::open(node.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd_ < 0) {
        if (err) *err = errno_msg("cannot open the window", node);
        return false;
    }
    unsigned long len = 0;
    if (::ioctl(fd_, kIocWindowLen, &len) != 0 || len == 0) {
        // A regular file does not answer the ioctl: its size is used. This allows testing
        // the reader against a simulated window and is used by the automatic tests.
        struct stat st {};
        if (::fstat(fd_, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            len = static_cast<unsigned long>(st.st_size);
        } else {
            if (err) *err = errno_msg("window length ioctl", node);
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }
    map_ = static_cast<std::uint8_t*>(::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd_, 0));
    if (map_ == MAP_FAILED) {
        map_ = nullptr;
        if (err) *err = errno_msg("window mmap", node);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    map_len_ = len;
    if (layout_.window_off + layout_.blocks * layout_.block_bytes > map_len_) {
        if (err) *err = "the ring does not fit in the mapped window";
        close();
        return false;
    }
    primed_ = false;
    last_seq_ = 0;
    blocks_read_ = blocks_lost_ = 0;
    last_info_ = IqBlockInfo{};
    return true;
}

void IqWindow::attach_for_test(const std::uint8_t* mem, std::size_t len) {
    close();
    if (!mem || layout_.window_off + layout_.blocks * layout_.block_bytes > len) return;
    map_ = const_cast<std::uint8_t*>(mem);
    map_len_ = len;
    primed_ = false;
    last_seq_ = 0;
    blocks_read_ = blocks_lost_ = 0;
    last_info_ = IqBlockInfo{};
}

void IqWindow::close() {
    if (map_) {
        ::munmap(map_, map_len_);
        map_ = nullptr;
        map_len_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

const std::uint8_t* IqWindow::block_ptr(std::size_t index) const {
    return map_ + layout_.window_off + index * layout_.block_bytes;
}

bool IqWindow::parse_block(const std::uint8_t* block, const IqRingLayout& layout, IqBlockInfo* out) {
    if (!block || !out) return false;
    const std::uint32_t sig = get_u32(block, kOffSig);
    IqSampleFormat fmt = format_from_magic(sig);
    if (fmt == IqSampleFormat::Unknown) return false;
    if (sig != layout.magic_legacy && sig != layout.magic_packed && sig != layout.magic_packed8)
        return false;

    IqBlockInfo info;
    info.bytes = get_u32(block, kOffBytes);
    if (info.bytes > layout.block_bytes - layout.header_bytes) return false;
    // The signature rules. The forced value only exists for old dumps that carry "CSD1"
    // with an 8-bit payload; it is never used on Device memory to guess the format.
    if (sig == layout.magic_packed && layout.force_format != IqSampleFormat::Unknown) {
        fmt = layout.force_format;
    }
    info.format = fmt;
    info.sample_bytes = sample_bytes(fmt);
    info.seq = get_u32(block, kOffSeq);
    info.ucnt = get_u32(block, kOffUcnt);
    info.state = get_u32(block, kOffState);
    if (is_packed(fmt)) {
        // Only the packed formats fill these fields; in CS16 they would be garbage.
        info.blk = get_u32(block, kOffBlk);
        info.push = get_u32(block, kOffPush);
        info.slot = get_u32(block, kOffSlot);
        info.p2 = get_u32(block, kOffP2);
        info.p3 = get_u32(block, kOffP3);
        info.stale = get_u32(block, kOffStale);
        info.freq_hz = freq_hz_from_p3(info.p3);
        info.next_freq_hz = freq_hz_from_p3(info.p2);
    }
    *out = info;
    return true;
}

bool IqWindow::parse_block(const std::uint8_t* block, const IqRingLayout& layout, std::uint32_t* seq,
                           std::uint32_t* ucnt, std::size_t* payload_bytes, std::uint32_t* state) {
    IqBlockInfo info;
    if (!parse_block(block, layout, &info)) return false;
    if (seq) *seq = info.seq;
    if (ucnt) *ucnt = info.ucnt;
    if (payload_bytes) *payload_bytes = info.bytes;
    if (state) *state = info.state;
    return true;
}

std::size_t IqWindow::read(void* dst, std::size_t max_bytes, IqBlockInfo* out_info) {
    if (!map_ || !dst || max_bytes == 0) return 0;

    std::size_t best = layout_.blocks;  // index of the newest valid block
    IqBlockInfo best_info;
    for (std::size_t i = 0; i < layout_.blocks; ++i) {
        IqBlockInfo info;
        if (!parse_block(block_ptr(i), layout_, &info)) continue;
        if (info.state != 0) continue;  // block still being written: not copied
        if (best == layout_.blocks || static_cast<std::int32_t>(info.seq - best_info.seq) > 0) {
            best = i;
            best_info = info;
        }
    }
    if (best == layout_.blocks) return 0;  // ring with no valid blocks yet

    const std::size_t best_len = best_info.bytes;
    if (!primed_) {  // first read: the newest block is delivered without counting losses
        primed_ = true;
    } else if (best_info.seq == last_seq_) {
        if (!repeat_last_ || best_len == 0) return 0;  // nothing new
        const std::size_t rep = best_len < max_bytes ? best_len : max_bytes;
        copy_from_window(static_cast<std::uint8_t*>(dst), block_ptr(best) + layout_.header_bytes, rep);
        last_info_ = best_info;
        if (out_info) *out_info = best_info;
        return rep;
    } else {
        const std::uint32_t advanced = best_info.seq - last_seq_;
        if (advanced > 1) blocks_lost_ += advanced - 1;
    }
    if (best_len == 0) return 0;

    const std::size_t n = best_len < max_bytes ? best_len : max_bytes;
    copy_from_window(static_cast<std::uint8_t*>(dst), block_ptr(best) + layout_.header_bytes, n);
    last_seq_ = best_info.seq;
    ++blocks_read_;
    last_info_ = best_info;
    if (out_info) *out_info = best_info;
    return n;
}

std::size_t IqWindow::read_next(void* dst, std::size_t max_bytes, IqBlockInfo* out_info) {
    if (!map_ || !dst || max_bytes == 0) return 0;

    // Among all valid blocks, the one with the lowest sequence above the last delivered is
    // chosen; that way a capture published across several consecutive blocks is drained
    // completely even if the poll arrives late. The first read starts at the oldest block
    // in the ring.
    std::size_t best = layout_.blocks;
    IqBlockInfo best_info;
    for (std::size_t i = 0; i < layout_.blocks; ++i) {
        IqBlockInfo info;
        if (!parse_block(block_ptr(i), layout_, &info)) continue;
        if (info.state != 0) continue;  // block still being written: not copied
        if (primed_ && static_cast<std::int32_t>(info.seq - last_seq_) <= 0) continue;  // already seen
        if (best == layout_.blocks || static_cast<std::int32_t>(info.seq - best_info.seq) < 0) {
            best = i;
            best_info = info;
        }
    }
    if (best == layout_.blocks) return 0;  // nothing new

    if (primed_) {
        const std::uint32_t advanced = best_info.seq - last_seq_;
        if (advanced > 1) blocks_lost_ += advanced - 1;
    } else {
        primed_ = true;  // first read: losses are not counted
    }
    const std::size_t best_len = best_info.bytes;
    if (best_len == 0) return 0;

    const std::size_t n = best_len < max_bytes ? best_len : max_bytes;
    copy_from_window(static_cast<std::uint8_t*>(dst), block_ptr(best) + layout_.header_bytes, n);
    last_seq_ = best_info.seq;
    ++blocks_read_;
    last_info_ = best_info;
    if (out_info) *out_info = best_info;
    return n;
}

IqWindow::RingProbe IqWindow::probe() const {
    RingProbe out;
    if (!map_) return out;
    for (std::size_t i = 0; i < layout_.blocks; ++i) {
        const std::uint8_t* b = block_ptr(i);
        // A slot that has never been published is all zeros: it is "nothing yet", not a format
        // error. Only a block with a payload and an unknown signature counts as no-format.
        const std::uint32_t bytes = get_u32(b, kOffBytes);
        if (bytes == 0) continue;
        const std::uint32_t sig = get_u32(b, kOffSig);
        IqBlockInfo info;
        if (parse_block(b, layout_, &info)) {
            ++out.blocks_valid;
            out.last_seq = info.seq;
        } else if (format_from_magic(sig) == IqSampleFormat::Unknown) {
            ++out.blocks_unknown;
            if (out.unknown_signature == 0) out.unknown_signature = sig;
        }
    }
    return out;
}

std::string IqWindow::description() const {
    char buf[224];
    std::snprintf(buf, sizeof(buf), "window off=0x%zx blocks=%zu block=0x%zx B signatures=%s/%s/%s",
                  layout_.window_off, layout_.blocks, layout_.block_bytes,
                  format_name(format_from_magic(layout_.magic_packed)),
                  format_name(format_from_magic(layout_.magic_packed8)),
                  format_name(format_from_magic(layout_.magic_legacy)));
    return std::string("iq-") + buf;
}

}  // namespace fmiq
