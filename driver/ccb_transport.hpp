// Driver receive transport: CCB META DL ring (MD -> AP) over mmap.
//
// The modem publishes pages in the DL ring of the CCB_META user (entry 19 of
// ccb_configs[]): 8 pages of 66560 B (66552 usable) at the base of the META block
// (+0x0). Each page starts with an 8 B header {u32 magic, u32 len};
// magic 0x11111111 while it is being written, 0x22222222 once published.
//
// Consume = read the page at dl_read_index, advance dl_read_index and then
// dl_free_index (ack to the modem) in the 64 B header of the control page.
// The consumer advances the read index after copying, like any producer/consumer ring.
//
// The region is mapped pgprot_noncached -> Device-nGnRnE on arm64: it does NOT accept
// unaligned accesses or memcpy. Copying here always uses aligned 32-bit reads (plus
// single bytes for the remainder), never memcpy from the ring.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace fmiq {

// Control header of a CCB user: 64 B = 16 32-bit words.
struct CcbRingInfo {
    uint32_t entry = 0;   // index in the control page (META = 19)
    uint32_t dlPage = 0;  // bytes per page (66560 in META)
    uint32_t dlPages = 0; // number of pages (8 in META)
    uint32_t dlSize = 0;  // bytes of the DL ring (532480 in META)
};

// DL ring reader over already mapped memory. Does not allocate per call
// (the only buffer, one page in size, is reserved in attach()).
// Not thread-safe: a single consumer thread.
class CcbRingReader {
public:
    static constexpr uint32_t kMagicBusy = 0x11111111u;
    static constexpr uint32_t kMagicValid = 0x22222222u;
    static constexpr uint32_t kGuardFirst = 0xddbbccaau;
    static constexpr uint32_t kGuardLast = 0xeeff0011u;
    static constexpr size_t kPageHdr = 8;

    // ctrlEntry: the 16 words of the entry in the control page.
    // dlBase:    base of the DL ring. info.dlPage/dlPages must be consistent.
    bool attach(volatile uint32_t* ctrlEntry, uint8_t* dlBase, const CcbRingInfo& info,
                std::string* err);
    void release();
    bool attached() const { return ctrl_ != nullptr && dl_ != nullptr; }

    // Copies up to maxBytes of the DL stream. Returns the bytes copied, or 0 if no page
    // is published (the caller decides the timeout).
    int read(void* dst, size_t maxBytes);

    uint32_t pendingPages() const;
    uint64_t droppedPages() const { return dropped_; }
    uint64_t badPages() const { return bad_; }
    uint32_t readIndex() const;
    uint32_t writeIndex() const;
    uint32_t freeIndex() const;
    // Header of page idx without consuming it (read-only diagnostics).
    bool peek(uint32_t idx, uint32_t& magic, uint32_t& len) const;

private:
    bool pullPage();
    uint32_t next(uint32_t i) const { return (i + 1u) % info_.dlPages; }

    volatile uint32_t* ctrl_ = nullptr;
    volatile uint8_t* dl_ = nullptr;
    CcbRingInfo info_{};
    std::unique_ptr<uint8_t[]> stage_;  // one payload page
    size_t stageLen_ = 0, stageOff_ = 0;
    uint64_t dropped_ = 0, bad_ = 0;
};

// Ring simulated in a file (to test the logic without a device).
// Layout: [4096 B control page][DL ring], with the user header at entry 19 as on the
// phone (META: DL 532480 B).
namespace simring {
constexpr size_t kCtrlBytes = 4096;
constexpr size_t kRingOffset = kCtrlBytes;
constexpr uint32_t kEntry = 19;
constexpr uint32_t kPages = 8;
constexpr uint32_t kPageSize = 66560;
constexpr size_t kRingBytes = static_cast<size_t>(kPages) * kPageSize;
constexpr size_t kFileBytes = kRingOffset + kRingBytes;

// Creates (or resets) the file with a valid header and zeroed indices.
bool create(const std::string& path, std::string* err);
}  // namespace simring

struct CcbOpenOptions {
    std::string node = "/dev/ccci_ccb_meta";
    std::string ctrl = "/dev/ccci_ccb_ctrl";
    // If not empty, a ring simulated in a file is used instead of the node.
    std::string simFile;
    int entryHint = -1;  // -1 = ask the node (CCB_CTRL_OFFSET ioctl)
    // The control node is mapped with PROT_WRITE to publish dl_read/dl_free.
    bool writableAck = true;
};

class CcbTransport {
public:
    // Opens the node (or the simulated file), maps the DL ring and validates the
    // header. Returns nullptr and fills err on failure.
    static std::unique_ptr<CcbTransport> open(const CcbOpenOptions& opt, std::string* err);

    int read(void* dst, size_t maxBytes) { return reader_.read(dst, maxBytes); }

    const CcbRingInfo& info() const { return info_; }
    const std::string& description() const { return desc_; }
    bool sim() const { return sim_; }
    uint32_t pendingPages() const { return reader_.pendingPages(); }
    uint64_t droppedPages() const { return reader_.droppedPages(); }
    uint64_t badPages() const { return reader_.badPages(); }
    uint32_t readIndex() const { return reader_.readIndex(); }
    uint32_t writeIndex() const { return reader_.writeIndex(); }
    uint32_t freeIndex() const { return reader_.freeIndex(); }
    bool peek(uint32_t idx, uint32_t& magic, uint32_t& len) const {
        return reader_.peek(idx, magic, len);
    }

    ~CcbTransport();

private:
    CcbTransport() = default;

    int fd_ = -1;
    int ctrlFd_ = -1;
    void* nodeMap_ = nullptr;
    size_t nodeMapLen_ = 0;
    void* ctrlMap_ = nullptr;
    size_t ctrlMapLen_ = 0;
    bool singleMap_ = false;  // sim: a single mmap covers control + ring
    CcbRingReader reader_;
    CcbRingInfo info_{};
    std::string desc_;
    bool sim_ = false;
};

}  // namespace fmiq
