#include "ccb_transport.hpp"

#include <atomic>
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

// CCB node ioctls (magic 'C'). The read ones are _IOR; the doorbell is the
// only _IOW used by the transport (_IOW('C', 0x32, u32) = 0x40044332).
constexpr unsigned long kIocSmemBase = 0x80044330UL;
constexpr unsigned long kIocSmemLen = 0x80044331UL;
constexpr unsigned long kIocCcbCtrlBase = 0x8004433DUL;
constexpr unsigned long kIocCcbCtrlLen = 0x8004433EUL;
constexpr unsigned long kIocCcbCtrlOffset = 0x80044341UL;

constexpr uint32_t kMaxPages = 1024;

// Copy from Device memory: aligned 32-bit reads + byte-by-byte tail.
// Never memcpy: on arm64 the ring is Device-nGnRnE and a wide unaligned access
// raises SIGBUS.
void copy_from_device(uint8_t* dst, const volatile uint8_t* src, size_t n) {
    const size_t n4 = n & ~static_cast<size_t>(3);
    for (size_t i = 0; i < n4; i += 4) {
        const uint32_t v = *reinterpret_cast<const volatile uint32_t*>(src + i);
        std::memcpy(dst + i, &v, sizeof v);
    }
    for (size_t i = n4; i < n; ++i) dst[i] = src[i];
}

bool u32_ioctl(int fd, unsigned long req, uint32_t& out) {
    uint32_t v = 0;
    if (::ioctl(fd, req, &v) < 0) return false;
    out = v;
    return true;
}

std::string errno_str(const char* what, const std::string& path) {
    return std::string(what) + " " + path + ": " + std::strerror(errno);
}

}  // namespace

// ---------------------------------------------------------------- CcbRingReader

bool CcbRingReader::attach(volatile uint32_t* ctrlEntry, uint8_t* dlBase,
                           const CcbRingInfo& info, std::string* err) {
    release();
    if (!ctrlEntry || !dlBase) {
        if (err) *err = "null ring header or base";
        return false;
    }
    if (info.dlPage < 16 || (info.dlPage % 4) != 0 || info.dlSize == 0 ||
        (info.dlSize % info.dlPage) != 0) {
        if (err) *err = "invalid DL ring geometry";
        return false;
    }
    const uint32_t pages = info.dlSize / info.dlPage;
    if (pages == 0 || pages > kMaxPages) {
        if (err) *err = "DL page count out of range";
        return false;
    }
    if (ctrlEntry[0] != kGuardFirst || ctrlEntry[7] != kGuardLast ||
        ctrlEntry[8] != kGuardFirst || ctrlEntry[15] != kGuardLast) {
        if (err) *err = "invalid CCB header guards";
        return false;
    }
    stage_.reset(new uint8_t[info.dlPage]);
    stageLen_ = stageOff_ = 0;
    dropped_ = bad_ = 0;
    ctrl_ = ctrlEntry;
    dl_ = reinterpret_cast<volatile uint8_t*>(dlBase);
    info_ = info;
    info_.dlPages = pages;
    return true;
}

void CcbRingReader::release() {
    ctrl_ = nullptr;
    dl_ = nullptr;
    stage_.reset();
    stageLen_ = stageOff_ = 0;
}

uint32_t CcbRingReader::pendingPages() const {
    if (!attached()) return 0;
    const uint32_t wr = ctrl_[4], rd = ctrl_[3];
    if (wr >= info_.dlPages || rd >= info_.dlPages) return 0;
    if (wr == rd) return 0;
    return (wr + info_.dlPages - rd) % info_.dlPages;
}

uint32_t CcbRingReader::readIndex() const { return attached() ? ctrl_[3] : 0; }
uint32_t CcbRingReader::writeIndex() const { return attached() ? ctrl_[4] : 0; }
uint32_t CcbRingReader::freeIndex() const { return attached() ? ctrl_[2] : 0; }

bool CcbRingReader::peek(uint32_t idx, uint32_t& magic, uint32_t& len) const {
    if (!attached() || idx >= info_.dlPages) return false;
    const volatile uint8_t* page = dl_ + static_cast<size_t>(idx) * info_.dlPage;
    magic = *reinterpret_cast<const volatile uint32_t*>(page);
    len = *reinterpret_cast<const volatile uint32_t*>(page + 4);
    return true;
}

bool CcbRingReader::pullPage() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t wr = ctrl_[4];
        const uint32_t rd = ctrl_[3];
        // The indices are modular (0..n-1), not free-running counters: a 32-bit
        // subtraction would give a huge value on every ring wrap.
        if (rd >= info_.dlPages || wr >= info_.dlPages) {  // corrupted indices
            ++dropped_;
            ctrl_[3] = 0;
            ctrl_[2] = 0;
            continue;
        }
        if (wr == rd) return false;  // empty (the producer always leaves one slot free)
        const volatile uint8_t* page = dl_ + static_cast<size_t>(rd) * info_.dlPage;
        const uint32_t magic = *reinterpret_cast<const volatile uint32_t*>(page);
        if (magic == kMagicBusy) return false;  // still being written: do not consume
        const uint32_t len = *reinterpret_cast<const volatile uint32_t*>(page + 4);
        if (magic != kMagicValid || len > info_.dlPage - kPageHdr) {
            ++bad_;
            const uint32_t nd = next(rd);  // release it so the ring does not jam
            ctrl_[3] = nd;
            ctrl_[2] = nd;
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        copy_from_device(stage_.get(), page + kPageHdr, len);
        std::atomic_thread_fence(std::memory_order_release);
        const uint32_t nd = next(rd);
        ctrl_[3] = nd;  // dl_read_index
        ctrl_[2] = nd;  // dl_free_index (ack to the modem)
        stageLen_ = len;
        stageOff_ = 0;
        return true;
    }
    return false;
}

int CcbRingReader::read(void* dst, size_t maxBytes) {
    if (!attached() || dst == nullptr || maxBytes == 0) return 0;
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < maxBytes) {
        if (stageOff_ < stageLen_) {  // rest of the page already copied
            size_t n = stageLen_ - stageOff_;
            if (n > maxBytes - done) n = maxBytes - done;
            std::memcpy(out + done, stage_.get() + stageOff_, n);
            stageOff_ += n;
            done += n;
            continue;
        }
        if (!pullPage()) break;  // no published pages
    }
    return static_cast<int>(done);
}

// ----------------------------------------------------------------- simring

namespace simring {

bool create(const std::string& path, std::string* err) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (err) *err = errno_str("cannot create", path);
        return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(kFileBytes)) < 0) {
        if (err) *err = errno_str("ftruncate", path);
        ::close(fd);
        return false;
    }
    void* base = ::mmap(nullptr, kFileBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        if (err) *err = errno_str("mmap", path);
        ::close(fd);
        return false;
    }
    std::memset(base, 0, kFileBytes);
    auto* w = static_cast<volatile uint32_t*>(base) + kEntry * 16;
    w[0] = CcbRingReader::kGuardFirst;
    w[1] = 0;  // dl_alloc
    w[2] = 0;  // dl_free
    w[3] = 0;  // dl_read
    w[4] = 0;  // dl_write
    w[5] = kPageSize;
    w[6] = static_cast<uint32_t>(kRingBytes);
    w[7] = CcbRingReader::kGuardLast;
    w[8] = CcbRingReader::kGuardFirst;
    w[9] = 0;   // reserved (not used by the receive path)
    w[10] = 0;  // reserved
    w[11] = 0;  // reserved
    w[12] = 0;  // reserved
    w[15] = CcbRingReader::kGuardLast;
    ::msync(base, kFileBytes, MS_SYNC);
    ::munmap(base, kFileBytes);
    ::close(fd);
    return true;
}

}  // namespace simring

// ---------------------------------------------------------------- CcbTransport

CcbTransport::~CcbTransport() {
    reader_.release();
    if (ctrlMap_ && !singleMap_) {
        ::munmap(ctrlMap_, ctrlMapLen_);
    }
    if (nodeMap_) ::munmap(nodeMap_, nodeMapLen_);
    if (ctrlFd_ >= 0) ::close(ctrlFd_);
    if (fd_ >= 0) ::close(fd_);
}

std::unique_ptr<CcbTransport> CcbTransport::open(const CcbOpenOptions& opt, std::string* err) {
    std::unique_ptr<CcbTransport> t(new CcbTransport());
    char buf[256];

    if (!opt.simFile.empty()) {
        // --- simulated ring in a file: [control 4096][DL ring] ---
        t->sim_ = true;
        t->fd_ = ::open(opt.simFile.c_str(), O_RDWR);
        if (t->fd_ < 0) {
            if (err) *err = errno_str("cannot open the simulated ring", opt.simFile);
            return nullptr;
        }
        struct stat st{};
        if (::fstat(t->fd_, &st) != 0 ||
            static_cast<size_t>(st.st_size) < simring::kFileBytes) {
            if (err) *err = "simulated ring too small (use simring::create)";
            return nullptr;
        }
        void* base = ::mmap(nullptr, simring::kFileBytes, PROT_READ | PROT_WRITE,
                            MAP_SHARED, t->fd_, 0);
        if (base == MAP_FAILED) {
            if (err) *err = errno_str("mmap", opt.simFile);
            return nullptr;
        }
        t->nodeMap_ = base;
        t->nodeMapLen_ = simring::kFileBytes;
        t->ctrlMap_ = base;
        t->ctrlMapLen_ = simring::kCtrlBytes;
        t->singleMap_ = true;
        auto* w = static_cast<volatile uint32_t*>(base) + simring::kEntry * 16;
        CcbRingInfo info;
        info.entry = simring::kEntry;
        info.dlPage = w[5];
        info.dlSize = w[6];
        info.dlPages = info.dlPage ? info.dlSize / info.dlPage : 0;
        uint8_t* ring = static_cast<uint8_t*>(base) + simring::kRingOffset;
        if (!t->reader_.attach(w, ring, info, err)) return nullptr;
        t->info_ = info;
        t->desc_ = "sim:" + opt.simFile;
        std::snprintf(buf, sizeof buf, "sim %u pages of %u B (%u B)",
                      info.dlPages, info.dlPage, info.dlSize);
        t->desc_ += " ";
        t->desc_ += buf;
        return t;
    }

    // --- real node ---
    t->fd_ = ::open(opt.node.c_str(), O_RDONLY | O_NONBLOCK);
    if (t->fd_ < 0) {
        if (err) *err = errno_str("cannot open the CCB node", opt.node);
        return nullptr;
    }
    const int ctrlWanted = opt.writableAck ? O_RDWR : O_RDONLY;
    t->ctrlFd_ = ::open(opt.ctrl.c_str(), ctrlWanted);
    if (t->ctrlFd_ < 0 && opt.writableAck) {  // retry without write access for diagnostics
        t->ctrlFd_ = ::open(opt.ctrl.c_str(), O_RDONLY);
    }
    if (t->ctrlFd_ < 0) {
        if (err) *err = errno_str("cannot open the CCB control page", opt.ctrl);
        return nullptr;
    }
    uint32_t smemBase = 0, smemLen = 0, ctrlBase = 0, ctrlLen = 0;
    if (!u32_ioctl(t->fd_, kIocSmemBase, smemBase) ||
        !u32_ioctl(t->fd_, kIocSmemLen, smemLen)) {
        if (err) *err = errno_str("ioctl SMEM_BASE/SMEM_LEN", opt.node);
        return nullptr;
    }
    if (!u32_ioctl(t->ctrlFd_, kIocCcbCtrlBase, ctrlBase) ||
        !u32_ioctl(t->ctrlFd_, kIocCcbCtrlLen, ctrlLen) || ctrlLen < 64) {
        if (err) *err = errno_str("ioctl CCB_CTRL_BASE/LEN", opt.ctrl);
        return nullptr;
    }
    t->ctrlMap_ = ::mmap(nullptr, ctrlLen, PROT_READ | PROT_WRITE, MAP_SHARED, t->ctrlFd_, 0);
    t->ctrlMapLen_ = ctrlLen;
    if (t->ctrlMap_ == MAP_FAILED) {
        t->ctrlMap_ = nullptr;
        // Without write access dl_read/dl_free cannot be published (ack to the modem).
        if (err) *err = errno_str("control page mmap", opt.ctrl) +
                        (opt.writableAck ? " (write permission is required for the ack)"
                                         : " (ack disabled)");
        return nullptr;
    }

    // index of the META entry in the control page
    uint32_t entry = 0;
    bool haveEntry = false;
    if (opt.entryHint >= 0) {
        entry = static_cast<uint32_t>(opt.entryHint);
        haveEntry = true;
    } else if (u32_ioctl(t->fd_, kIocCcbCtrlOffset, entry)) {
        haveEntry = true;
    }
    const uint32_t maxEntries = ctrlLen / 64;
    auto entryPtr = [&](uint32_t i) {
        auto* base = static_cast<volatile uint32_t*>(t->ctrlMap_);
        return base + static_cast<size_t>(i) * 16;
    };
    auto guardOk = [&](uint32_t i) {
        if (i >= maxEntries) return false;
        volatile uint32_t* w = entryPtr(i);
        return w[0] == CcbRingReader::kGuardFirst && w[7] == CcbRingReader::kGuardLast;
    };
    if (haveEntry && (entry >= maxEntries || !guardOk(entry))) haveEntry = false;
    if (!haveEntry) {  // look for the META entry by its geometry
        for (uint32_t i = 0; i < maxEntries; ++i) {
            if (!guardOk(i)) continue;
            volatile uint32_t* w = entryPtr(i);
            if (w[5] == simring::kPageSize && w[6] == simring::kRingBytes) {
                entry = i;
                haveEntry = true;
                break;
            }
        }
    }
    if (!haveEntry) {
        if (err) *err = "META entry not found in the control page";
        return nullptr;
    }

    volatile uint32_t* w = entryPtr(entry);
    CcbRingInfo info;
    info.entry = entry;
    info.dlPage = w[5];
    info.dlSize = w[6];
    info.dlPages = info.dlPage ? info.dlSize / info.dlPage : 0;
    if (info.dlSize == 0 || info.dlSize > smemLen) {
        if (err) *err = "dl_size inconsistent with the SMEM region";
        return nullptr;
    }
    // The ioctl returns the whole SMEM region (2 MiB), but this user only maps the DL
    // ring; mapping more can raise SIGBUS on pages with no backing store.
    const size_t mapLen = info.dlSize;
    t->nodeMap_ = ::mmap(nullptr, mapLen, PROT_READ, MAP_SHARED, t->fd_, 0);
    t->nodeMapLen_ = mapLen;
    if (t->nodeMap_ == MAP_FAILED) {
        t->nodeMap_ = nullptr;
        if (err) *err = errno_str("ring mmap", opt.node);
        return nullptr;
    }
    if (!t->reader_.attach(w, static_cast<uint8_t*>(t->nodeMap_), info, err)) return nullptr;
    t->info_ = info;
    std::snprintf(buf, sizeof buf, "%s entry=%u dl=%u/%u B base=0x%08x ctrl=0x%08x",
                  opt.node.c_str(), entry, info.dlSize, info.dlPage, smemBase, ctrlBase);
    t->desc_ = buf;
    return t;
}

}  // namespace fmiq
