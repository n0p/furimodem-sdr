// What the picture on screen is made of: a capture published by the receiver now, a capture that
// is being repeated, or nothing.
//
// The block header already carries `seq`, which the firmware advances with every published
// capture (see common/iq_format.hpp), and the pipeline sees it block by block. The app used to
// show only a counter of dropped blocks ("D"), which grows by design and says nothing about the
// age of the data. This turns the sequence into an age that can be read at a glance:
//
//   LIVE      a new seq arrived within kCaptureFreshS: the receiver is publishing now
//   REPEAT    samples still arrive but seq does not advance: the last capture is repeated
//             (repeat_last in the module, a looping recording...)
//   NO DATA   nothing has arrived for kCaptureStoppedS
//   GEN       the source has no block header (synthetic): every block is generated on the spot
//
// The functions take the current time as an argument so the decision is testable without a clock.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace fmiq {

enum class CaptureKind {
    Live,
    Repeated,
    Stopped,
    Generated,
};

// A new capture at least this often is "live". The ring publishes one capture every few tens of
// ms; 0.5 s leaves room for a slow sweep without turning a live stream into "repeat".
inline constexpr double kCaptureFreshS = 0.5;
// No block at all for this long: the stream has stopped (or never started).
inline constexpr double kCaptureStoppedS = 1.0;

struct CaptureState {
    CaptureKind kind = CaptureKind::Generated;
    double age_s = 0.0;       // seconds since the last capture with a new seq (or since the last block)
    std::uint32_t seq = 0;
    bool has_seq = false;

    bool live() const { return kind == CaptureKind::Live || kind == CaptureKind::Generated; }
};

// Does this block carry a capture the receiver has just published? `first` is the first block of
// the stream. A new seq always counts. The block counter counts only when the header carries no
// sequence at all (some images leave seq at 0), so a repeated block is never mistaken for a new
// capture just because it was read again.
inline bool is_new_capture(bool first, std::uint32_t seq, std::uint32_t prev_seq,
                           std::uint32_t blocks, std::uint32_t prev_blocks) {
    if (first) return true;
    if (seq != prev_seq) return true;
    return seq == 0 && blocks != prev_blocks;
}

// `now_s`, `last_block_s` and `last_new_s` are seconds on the same monotonic clock. A negative
// value in the last two means "never".
inline CaptureState capture_state(bool has_meta, bool has_seq, std::uint32_t seq, double now_s,
                                  double last_block_s, double last_new_s) {
    CaptureState s;
    s.seq = seq;
    s.has_seq = has_seq;
    const double since_block = last_block_s >= 0.0 ? now_s - last_block_s : 0.0;
    if (!has_meta) {
        s.kind = CaptureKind::Generated;
        s.age_s = since_block > 0.0 ? since_block : 0.0;
        return s;
    }
    if (last_block_s < 0.0 || since_block > kCaptureStoppedS) {
        s.kind = CaptureKind::Stopped;
        s.age_s = last_block_s >= 0.0 ? since_block : 0.0;
        return s;
    }
    const double since_new = last_new_s >= 0.0 ? now_s - last_new_s : since_block;
    s.age_s = since_new > 0.0 ? since_new : 0.0;
    s.kind = since_new > kCaptureFreshS ? CaptureKind::Repeated : CaptureKind::Live;
    return s;
}

// Short label for the status bar. One line, ASCII only (the UI font is 5x7).
inline std::string capture_label(const CaptureState& s) {
    char buf[48];
    switch (s.kind) {
        case CaptureKind::Live:
            std::snprintf(buf, sizeof(buf), "LIVE %.1fS%s%u", s.age_s, s.has_seq ? " #" : "",
                          s.has_seq ? s.seq : 0u);
            break;
        case CaptureKind::Repeated:
            std::snprintf(buf, sizeof(buf), "REPEAT %.1fS%s%u", s.age_s, s.has_seq ? " #" : "",
                          s.has_seq ? s.seq : 0u);
            break;
        case CaptureKind::Stopped:
            std::snprintf(buf, sizeof(buf), "NO DATA %.1fS", s.age_s);
            break;
        case CaptureKind::Generated:
        default:
            std::snprintf(buf, sizeof(buf), "GEN %.1fS", s.age_s);
            break;
    }
    return std::string(buf);
}

}  // namespace fmiq
