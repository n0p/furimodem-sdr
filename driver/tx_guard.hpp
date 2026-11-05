// TX policy for the driver. This is the safety-critical piece: it is tested on its own and
// without hardware.
// Public build: three interlocks. Laboratory build: only the parameter plus risky bands aside.
#pragma once
#include <cstdint>
#include <string>
#include <cmath>

namespace fmiq {

enum class Build { Public, Lab };

struct TxPolicy {
    Build build = Build::Public;

    // Interlock 1: without this there is no TX channel.
    bool tx_enable = false;
    // Interlock 2: the stream must be armed explicitly.
    bool tx_armed = false;
    // Interlock 3 (public build only): hard limits.
    double max_gain_db = 10.0;          // public; ignored in lab (0 = no cap)
    double min_freq_hz = 2400e6;        // public: 2.4 GHz only
    double max_freq_hz = 2500e6;
    std::size_t max_samples_per_write = 1u << 16;
    // Laboratory: bands where transmitting can damage the front end.
    bool allow_risky_bands = false;
    // Hardware range: physical limit, applies to both builds.
    double hw_min_freq_hz = 400e6;
    double hw_max_freq_hz = 6000e6;
    double hw_max_gain_db = 40.0;
};

enum class Verdict { Ok, NoTxChannel, NotArmed, FreqOutOfRange, RiskyBand, GainTooHigh, TooManySamples };

inline const char* verdict_str(Verdict v) {
    switch (v) {
        case Verdict::Ok: return "ok";
        case Verdict::NoTxChannel: return "tx channel does not exist (tx_enable=0)";
        case Verdict::NotArmed: return "tx stream not armed";
        case Verdict::FreqOutOfRange: return "frequency outside allowed range";
        case Verdict::RiskyBand: return "band requires allow_risky_bands=1";
        case Verdict::GainTooHigh: return "gain above allowed maximum";
        case Verdict::TooManySamples: return "too many samples per write";
    }
    return "?";
}

// Bands marked as unsafe for transmission (no calibrated TX path / PA or filter not provided for).
// They are declared here as ranges and the laboratory build can bypass them with allow_risky_bands.
inline bool is_risky_band(double hz) {
    struct R { double lo, hi; };
    static const R risky[] = {
        {698e6, 960e6},     // sub-1 GHz: no validated TX path on this front end
        {1710e6, 2200e6},   // lower high bands
        {3300e6, 4200e6},   // C-band
        {5150e6, 5925e6},   // 5 GHz
    };
    for (const auto& r : risky) if (hz >= r.lo && hz <= r.hi) return true;
    return false;
}

class TxGuard {
public:
    explicit TxGuard(TxPolicy p) : p_(p) {}

    const TxPolicy& policy() const { return p_; }
    bool tx_channel_exists() const { return p_.tx_enable; }

    Verdict check_frequency(double hz) const {
        if (!p_.tx_enable) return Verdict::NoTxChannel;
        if (hz < p_.hw_min_freq_hz || hz > p_.hw_max_freq_hz) return Verdict::FreqOutOfRange;
        if (p_.build == Build::Public) {
            if (hz < p_.min_freq_hz || hz > p_.max_freq_hz) return Verdict::FreqOutOfRange;
        } else if (!p_.allow_risky_bands && is_risky_band(hz)) {
            return Verdict::RiskyBand;
        }
        return Verdict::Ok;
    }

    Verdict check_gain(double db) const {
        if (!p_.tx_enable) return Verdict::NoTxChannel;
        const double cap = (p_.build == Build::Public) ? p_.max_gain_db : p_.hw_max_gain_db;
        if (db > cap) return Verdict::GainTooHigh;
        return Verdict::Ok;
    }

    Verdict check_write(std::size_t n) const {
        if (!p_.tx_enable) return Verdict::NoTxChannel;
        if (!p_.tx_armed) return Verdict::NotArmed;
        if (n > p_.max_samples_per_write) return Verdict::TooManySamples;
        return Verdict::Ok;
    }

    // Clamps instead of failing when the limit is about size (the returned value records it).
    std::size_t clamp_write(std::size_t n) const {
        return n > p_.max_samples_per_write ? p_.max_samples_per_write : n;
    }

    void arm() { p_.tx_armed = true; }
    void disarm() { p_.tx_armed = false; }

    std::string banner() const {
        if (p_.build == Build::Lab) {
            return "LAB BUILD: TX unrestricted (only tx_enable gates it)" +
                   std::string(p_.allow_risky_bands ? "; risky bands ALLOWED" : "; risky bands blocked");
        }
        return "PUBLIC BUILD: TX limited to 2.4 GHz and low gain";
    }

private:
    TxPolicy p_;
};

} // namespace fmiq
