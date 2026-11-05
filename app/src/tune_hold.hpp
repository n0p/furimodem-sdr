// User tuning versus the frequency carried by each block.
//
// A source with its own block header (the modem ring or a block file) reports the burst
// frequency and rate in every block. Those values must not overwrite a value the user has just
// set from the controls: at 50-100 blocks/s the header would win 10-20 ms later and the buttons
// would look dead. This header keeps a short per-field hold over the values set by the user and,
// when it expires, decides whether the source followed the request or ignored it. The interface
// uses that verdict to say so on screen instead of pretending the control worked.
#pragma once

#include "source.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace fmiq {

// How long a value set from the controls is kept before the block header may take over again.
inline constexpr double kUserHoldSeconds = 2.0;
// The header frequency is quantized to 100 kHz: anything closer than this is the same tune.
inline constexpr double kTuneMatchHz = 50e3;

inline int bandForFreq(double hz) {
    for (int i = 0; i < kBandCount; ++i)
        if (hz >= kBands[i].lo_hz && hz <= kBands[i].hi_hz) return i;
    return -1;
}

struct TuneHold {
    using Clock = std::chrono::steady_clock;

    // Values set from the controls that the block header must not overwrite yet.
    bool center_pending = false;
    bool rate_pending = false;
    Clock::time_point center_deadline{};
    Clock::time_point rate_deadline{};
    double center_hz = 0.0;
    double rate_hz = 0.0;

    // Last request, kept after the hold expires: it is what the notice talks about.
    double requested_hz = 0.0;
    double requested_rate = 0.0;
    bool tune_rejected = false;  // the source did not deliver the requested frequency
    bool rate_rejected = false;  // the source did not deliver the requested rate

    // Records a change made by the user (the store must already contain the new values).
    // Gain changes are not held: no source header carries a gain.
    void noteUser(const RxParams& before, const RxParams& after, Clock::time_point now = Clock::now()) {
        if (std::fabs(after.center_hz - before.center_hz) > 0.5) {
            center_pending = true;
            center_hz = after.center_hz;
            requested_hz = after.center_hz;
            center_deadline = now + duration(kUserHoldSeconds);
            tune_rejected = false;  // new request: verdict pending again
        }
        if (std::fabs(after.sample_rate - before.sample_rate) > 0.5) {
            rate_pending = true;
            rate_hz = after.sample_rate;
            requested_rate = after.sample_rate;
            rate_deadline = now + duration(kUserHoldSeconds);
            rate_rejected = false;
        }
    }

    bool holdingCenter(Clock::time_point now) const { return center_pending && now < center_deadline; }
    bool holdingRate(Clock::time_point now) const { return rate_pending && now < rate_deadline; }

private:
    static Clock::duration duration(double seconds) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    }
};

// Applies one block header to the parameters. Returns true if anything changed.
//
// With hold == nullptr it behaves exactly as before: the block always wins (headless and
// gpu-test do not have a user touching the controls). With a TuneHold:
//   - while a user value is held, the header does not touch that field;
//   - if the header already agrees with the request, the module took it and the hold ends;
//   - when the hold expires without agreement, the header wins again (the axis must be truthful)
//     and the rejection flag is raised for the interface to report.
inline bool followSourceMeta(ParamStore& params, const SourceMeta& m, TuneHold* hold = nullptr,
                             TuneHold::Clock::time_point now = TuneHold::Clock::now()) {
    if (!m.valid || !m.follows_freq) return false;
    RxParams p = params.get();
    bool changed = false;

    if (m.center_hz > 0) {
        bool apply = true;
        if (hold) {
            if (hold->center_pending) {
                if (std::fabs(m.center_hz - hold->center_hz) <= kTuneMatchHz) {
                    // The source delivers what was asked: the request went through.
                    hold->center_pending = false;
                    hold->tune_rejected = false;
                } else if (hold->holdingCenter(now)) {
                    apply = false;  // do not let the header overwrite the user's value
                } else {
                    hold->center_pending = false;
                    hold->tune_rejected = true;  // ignored: follow the block and report it
                }
            } else if (hold->tune_rejected &&
                       std::fabs(m.center_hz - hold->requested_hz) <= kTuneMatchHz) {
                hold->tune_rejected = false;  // it reached the request after all (e.g. a sweep)
            }
        }
        if (apply && std::fabs(m.center_hz - p.center_hz) > 0.5) {
            p.center_hz = m.center_hz;
            const int nb = bandForFreq(m.center_hz);
            if (nb >= 0) p.band = nb;
            changed = true;
        }
    }

    if (m.sample_rate > 0) {
        bool apply = true;
        if (hold) {
            if (hold->rate_pending) {
                if (std::fabs(m.sample_rate - hold->rate_hz) <= 1.0) {
                    hold->rate_pending = false;
                    hold->rate_rejected = false;
                } else if (hold->holdingRate(now)) {
                    apply = false;
                } else {
                    hold->rate_pending = false;
                    hold->rate_rejected = true;
                }
            } else if (hold->rate_rejected &&
                       std::fabs(m.sample_rate - hold->requested_rate) <= 1.0) {
                hold->rate_rejected = false;
            }
        }
        if (apply && std::fabs(m.sample_rate - p.sample_rate) > 0.5) {
            p.sample_rate = m.sample_rate;
            changed = true;
        }
    }

    if (changed) params.set(p);
    return changed;
}

// Text for the amber notice: why the control just pressed cannot move the receiver. It is built
// from the last header read-back, so it is true for a fixed profile and for a sweep. Empty means
// there is nothing to say (the source follows the controls, or nothing was requested).
inline std::string tuningNotice(const TuneHold& hold, const SourceMeta& m, bool device,
                                const RxParams& requested) {
    if (!hold.tune_rejected && !hold.rate_rejected) return std::string();
    const double rx_hz = m.center_hz > 0 ? m.center_hz : requested.center_hz;
    const double rx_rate = m.sample_rate > 0 ? m.sample_rate : requested.sample_rate;
    char buf[256] = "";
    if (hold.tune_rejected) {
        if (device)
            std::snprintf(buf, sizeof(buf),
                          "TUNING IS CONTROLLED BY THE ACTIVATED PROFILE; RECEIVER AT %.3f MHZ",
                          rx_hz / 1e6);
        else
            std::snprintf(buf, sizeof(buf),
                          "TUNING COMES FROM THE BLOCK HEADER; STREAM CENTRED AT %.3f MHZ",
                          rx_hz / 1e6);
    }
    std::string out = buf;
    if (hold.rate_rejected) {
        char rb[96];
        std::snprintf(rb, sizeof(rb), "%sWIDTH FIXED AT %.2f MS/S",
                      out.empty() ? "" : " | ", rx_rate / 1e6);
        out += rb;
    }
    return out;
}

}  // namespace fmiq
