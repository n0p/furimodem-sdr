// Tests for the TX policy (no hardware).
#include "tx_guard.hpp"
#include <cstdio>
#include <string>

using namespace fmiq;

static int fails = 0;
static void expect(bool cond, const std::string& what) {
    std::printf("%-58s %s\n", what.c_str(), cond ? "OK" : "FAIL");
    if (!cond) ++fails;
}

int main() {
    // --- Public build ---
    {
        TxPolicy p; p.build = Build::Public;      // tx_enable = false by default
        TxGuard g(p);
        expect(!g.tx_channel_exists(), "public: without tx_enable there is no TX channel");
        expect(g.check_frequency(2450e6) == Verdict::NoTxChannel, "public: TX rejected without tx_enable");

        p.tx_enable = true; TxGuard g2(p);
        expect(g2.check_frequency(2450e6) == Verdict::Ok, "public: 2450 MHz accepted");
        expect(g2.check_frequency(2600e6) == Verdict::FreqOutOfRange, "public: 2600 MHz rejected");
        expect(g2.check_gain(40.0) == Verdict::GainTooHigh, "public: 40 dB rejected");
        expect(g2.check_write(1024) == Verdict::NotArmed, "public: unarmed write rejected");
        g2.arm();
        expect(g2.check_write(1024) == Verdict::Ok, "public: armed write accepted");
        expect(g2.check_write(1u << 20) == Verdict::TooManySamples, "public: huge block rejected");
        expect(g2.clamp_write(1u << 20) == (1u << 16), "public: huge block clamped to the maximum");
        expect(g2.check_frequency(700e6) == Verdict::FreqOutOfRange, "public: risky band also out of range");
    }

    // --- Laboratory build ---
    {
        TxPolicy p; p.build = Build::Lab; p.tx_enable = true; TxGuard g(p);
        expect(g.check_frequency(2600e6) == Verdict::Ok, "lab: 2600 MHz accepted (no policy cap)");
        expect(g.check_frequency(2450e6) == Verdict::Ok, "lab: 2450 MHz accepted");
        expect(g.check_gain(30.0) == Verdict::Ok, "lab: 30 dB accepted");
        expect(g.check_gain(45.0) == Verdict::GainTooHigh, "lab: above the hardware limit rejected");
        expect(g.check_frequency(800e6) == Verdict::RiskyBand, "lab: risky band blocked by default");
        expect(g.check_frequency(3800e6) == Verdict::RiskyBand, "lab: C-band blocked by default");
        expect(g.check_frequency(7000e6) == Verdict::FreqOutOfRange, "lab: outside the hardware rejected");

        p.allow_risky_bands = true; TxGuard g2(p);
        expect(g2.check_frequency(800e6) == Verdict::Ok, "lab+allow_risky: 800 MHz allowed");
        expect(g2.banner().find("risky bands ALLOWED") != std::string::npos, "lab: banner warns about risky bands");
    }

    std::printf("\n%s (%d failure(s))\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
