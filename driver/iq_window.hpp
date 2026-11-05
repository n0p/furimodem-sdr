// Reader for the IQ ring that the firmware leaves in shared memory.
//
// The module does not speak any transport protocol: it only copies blocks that have
// already been captured. The window is opened read-only and every block carries a
// header with a sequence number, so the client detects new blocks and ring wraps
// without ambiguity.
//
// The header (0x80 B) is shared by the three formats and the signature says which one
// it is: "CSD1" carries 5-bit I/Q (one sample per 16-bit word), "CSD8" 8-bit I/Q (I in
// the low byte, Q in the high byte) and "RF30" is the earlier CS16. Both packed
// variants carry the burst frequency in p3. See common/iq_format.hpp.
#pragma once

#include "iq_format.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fmiq {

// Ring geometry. The defaults match the firmware that writes it; they can be changed
// through device arguments for other variants.
struct IqRingLayout {
    std::size_t window_off = 0x1D81000;  // ring offset inside the window
    std::size_t blocks = 16;             // number of blocks
    std::size_t block_bytes = 16384;     // capacity of each block (header included)
    std::size_t header_bytes = kBlockHeaderBytes;
    std::uint32_t magic_legacy = kMagicRf30;   // CS16 (previous format)
    std::uint32_t magic_packed = kMagicCsd1;   // packed 5-bit I/Q
    std::uint32_t magic_packed8 = kMagicCsd8;  // packed 8-bit I/Q
    // Forced format ONLY for "CSD1" blocks without the flag (dumps predating "CSD8").
    // Unknown = the signature decides; "CSD8" is never rewritten.
    IqSampleFormat force_format = IqSampleFormat::Unknown;
};

// Metadata of one block. Besides what the header already carried (seq, bytes, state)
// the sample format and the burst frequency are decoded.
struct IqBlockInfo {
    std::uint32_t seq = 0;
    std::uint32_t ucnt = 0;
    std::uint32_t bytes = 0;
    std::uint32_t state = 0;
    std::uint32_t blk = 0;       // 0/1: half of the capture
    std::uint32_t push = 0;      // scanner counter
    std::uint32_t slot = 0;      // pool slot
    std::uint32_t p2 = 0;        // page+8: next frequency of the sweep (high 16, 100 kHz)
    std::uint32_t p3 = 0;        // page+12: burst frequency (high 16, 100 kHz)
    std::uint32_t stale = 0;     // the slot may have been reassigned before being copied
    std::uint64_t freq_hz = 0;   // decoded p3; 0 if the field is not valid
    std::uint64_t next_freq_hz = 0;
    IqSampleFormat format = IqSampleFormat::Unknown;
    std::size_t sample_bytes = 0;

    bool valid() const { return format != IqSampleFormat::Unknown && bytes > 0; }
};

class IqWindow {
public:
    explicit IqWindow(IqRingLayout layout = {});
    ~IqWindow();
    IqWindow(const IqWindow&) = delete;
    IqWindow& operator=(const IqWindow&) = delete;

    // Opens and maps the modem memory window read-only.
    bool open(const std::string& node, std::string* err);
    void close();
    bool is_open() const { return map_ != nullptr; }

    // Copies the most recent block the client has not seen yet. Returns the number of
    // sample bytes copied (0 if there is nothing new) and updates the lost-block counter.
    // If info is passed, it is filled with the header of the delivered block (format and
    // frequency).
    std::size_t read(void* dst, std::size_t max_bytes, IqBlockInfo* info = nullptr);

    // Same, but delivers blocks IN ORDER: the oldest one the client has not seen. The
    // product may publish one capture across several consecutive blocks; with read() a slow
    // poll keeps the last one and loses the earlier ones. read_next() drains all of them.
    std::size_t read_next(void* dst, std::size_t max_bytes, IqBlockInfo* info = nullptr);

    // Header of the last delivered block (or invalid if there is none yet).
    const IqBlockInfo& last_block() const { return last_info_; }

    std::uint32_t blocks_read() const { return blocks_read_; }
    std::uint32_t blocks_lost() const { return blocks_lost_; }
    std::uint32_t last_seq() const { return last_seq_; }
    std::string description() const;

    // Read-only snapshot for diagnostics (readSetting("rx_ring")): how many slots carry a
    // complete block with a signature this driver knows, and how many carry a header it cannot
    // parse. It does not consume anything and changes no state.
    struct RingProbe {
        std::uint32_t blocks_valid = 0;      // complete blocks with a known signature
        std::uint32_t blocks_unknown = 0;    // blocks published with an unknown signature
        std::uint32_t unknown_signature = 0; // first unknown signature seen (for the message)
        std::uint32_t last_seq = 0;          // newest sequence among the valid blocks
    };
    RingProbe probe() const;

    // With repeat_last, if there is no new block the last one read is delivered again. It
    // keeps the display alive after a burst; the data is still the captured data.
    void set_repeat_last(bool on) { repeat_last_ = on; }

    // Tests only: attaches already mapped memory (no node, no ioctl).
    void attach_for_test(const std::uint8_t* mem, std::size_t len);

    // Parses a block already in memory. Returns false if the signature is unknown or the
    // length does not fit; in that case the output is left untouched.
    static bool parse_block(const std::uint8_t* block, const IqRingLayout& layout, IqBlockInfo* info);

    // Compatibility with the earlier reader (common fields only).
    static bool parse_block(const std::uint8_t* block, const IqRingLayout& layout, std::uint32_t* seq,
                            std::uint32_t* ucnt, std::size_t* payload_bytes,
                            std::uint32_t* state = nullptr);

private:
    const std::uint8_t* block_ptr(std::size_t index) const;

    IqRingLayout layout_;
    int fd_ = -1;
    std::uint8_t* map_ = nullptr;
    std::size_t map_len_ = 0;
    std::uint32_t last_seq_ = 0;
    std::uint32_t blocks_read_ = 0;
    std::uint32_t blocks_lost_ = 0;
    bool primed_ = false;
    bool repeat_last_ = false;
    IqBlockInfo last_info_;
};

}  // namespace fmiq
