// Unit test for the control hold (tune_hold.hpp). It exercises the exact bug reported on the
// phone without a display or a device: a block header that carries 2115.0 MHz used to overwrite
// whatever band or ±1M the user had just selected, so the buttons looked dead.
#include "tune_hold.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace fmiq;
using Clock = TuneHold::Clock;
using namespace std::chrono;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++failures;
}

void checkNear(double got, double want, double tol, const char* what) {
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("%s  %s (%.6f MHz)\n", ok ? "OK  " : "FAIL", what, got / 1e6);
    if (!ok) ++failures;
}

SourceMeta block(double center_hz, double rate_hz) {
    SourceMeta m;
    m.valid = true;
    m.follows_freq = true;
    m.center_hz = center_hz;
    m.sample_rate = rate_hz;
    m.format = IqSampleFormat::Packed5;
    return m;
}

// A block as the current firmware publishes it: fixed at 2115.0 MHz, 1.92 MS/s.
SourceMeta fixedProfile() { return block(2115.0e6, 1.92e6); }

}  // namespace

int main() {
    const auto t0 = Clock::now();

    // 1. The header must not overwrite a band just selected by the user.
    {
        ParamStore params;
        RxParams base = params.get();
        base.sample_rate = 1.92e6;  // the recorded profile rate, so only the centre is under test
        params.set(base);
        TuneHold hold;
        RxParams p = base;
        p.band = 1;  // B3
        p.center_hz = kBands[1].center_hz;
        const RxParams before = params.get();
        params.set(p);
        hold.noteUser(before, p, t0);

        const bool changed = followSourceMeta(params, fixedProfile(), &hold, t0 + milliseconds(20));
        check(!changed, "block header changes nothing while the user hold is active");
        checkNear(params.get().center_hz, kBands[1].center_hz, 1.0, "band centre survives the header");
        check(params.get().band == 1, "selected band survives the header");
        check(!hold.tune_rejected, "no rejection is reported before the hold expires");
    }

    // 2. +1M: the offset is visible during the hold, then the block wins and the rejection is real.
    {
        ParamStore params;
        TuneHold hold;
        const RxParams before = params.get();
        RxParams p = before;
        p.center_hz += 1.0e6;
        params.set(p);
        hold.noteUser(before, p, t0);

        followSourceMeta(params, fixedProfile(), &hold, t0 + milliseconds(1000));
        checkNear(params.get().center_hz, before.center_hz + 1.0e6, 1.0, "+1M is kept for the whole hold");

        const bool changed = followSourceMeta(params, fixedProfile(), &hold, t0 + seconds(3));
        check(changed, "after the hold the axis follows the block again");
        checkNear(params.get().center_hz, 2115.0e6, 1.0, "receiver frequency taken from the header");
        check(hold.tune_rejected, "the ignored request is flagged for the interface");
        check(!hold.center_pending, "the pending request is settled");
    }

    // 3. A source that does follow the request must not raise a notice.
    {
        ParamStore params;
        RxParams base = params.get();
        base.sample_rate = 1.92e6;
        params.set(base);
        TuneHold hold;
        const RxParams before = params.get();
        RxParams p = before;
        p.band = 3;  // B20
        p.center_hz = kBands[3].center_hz;
        params.set(p);
        hold.noteUser(before, p, t0);

        const bool changed = followSourceMeta(params, block(kBands[3].center_hz, 1.92e6), &hold,
                                              t0 + milliseconds(40));
        check(!changed, "header already carries the requested frequency");
        check(!hold.center_pending, "hold released as soon as the request is observed");
        check(!hold.tune_rejected, "accepted request leaves no notice");
    }

    // 4. Width: 15.36 MS/s is held, then the fixed 1.92 MS/s of the ring wins and is reported.
    {
        ParamStore params;
        TuneHold hold;
        const RxParams before = params.get();
        RxParams p = before;
        p.sample_rate = 15.36e6;
        params.set(p);
        hold.noteUser(before, p, t0);

        followSourceMeta(params, fixedProfile(), &hold, t0 + milliseconds(500));
        checkNear(params.get().sample_rate, 15.36e6, 1.0, "selected width is kept during the hold");

        followSourceMeta(params, fixedProfile(), &hold, t0 + seconds(3));
        checkNear(params.get().sample_rate, 1.92e6, 1.0, "block rate wins after the hold");
        check(hold.rate_rejected, "fixed width is flagged for the interface");
    }

    // 5. Headless and gpu-test pass no hold: the old behaviour (block always wins) is intact.
    {
        ParamStore params;
        const bool changed = followSourceMeta(params, fixedProfile());
        check(changed, "without a hold the block is still applied immediately");
        checkNear(params.get().center_hz, 2115.0e6, 1.0, "block centre applied without a hold");
    }

    // 6. A new request clears the previous rejection verdict while it is being evaluated.
    {
        ParamStore params;
        TuneHold hold;
        RxParams before = params.get();
        RxParams p = before;
        p.center_hz += 1.0e6;
        params.set(p);
        hold.noteUser(before, p, t0);
        followSourceMeta(params, fixedProfile(), &hold, t0 + seconds(3));
        check(hold.tune_rejected, "first request flagged");

        before = params.get();
        p = before;
        p.center_hz += 1.0e6;
        params.set(p);
        hold.noteUser(before, p, t0 + seconds(4));
        check(!hold.tune_rejected, "a new request starts clean");
        check(hold.center_pending, "the new request is held");
    }

    // 7. If a sweeping source later reaches the requested frequency, the notice clears.
    {
        ParamStore params;
        TuneHold hold;
        const RxParams before = params.get();
        RxParams p = before;
        p.center_hz = 1842.5e6;
        params.set(p);
        hold.noteUser(before, p, t0);
        followSourceMeta(params, fixedProfile(), &hold, t0 + seconds(3));
        check(hold.tune_rejected, "request to another band is flagged");
        followSourceMeta(params, block(1842.5e6, 1.92e6), &hold, t0 + seconds(4));
        check(!hold.tune_rejected, "notice clears when the source reaches the request");
    }

    // 8. The notice text names the actual frequency and distinguishes device from file.
    {
        ParamStore params;
        RxParams base = params.get();
        base.sample_rate = 1.92e6;
        params.set(base);
        TuneHold hold;
        const RxParams before = params.get();
        RxParams p = before;
        p.band = 1;
        p.center_hz = kBands[1].center_hz;
        params.set(p);
        hold.noteUser(before, p, t0);
        followSourceMeta(params, fixedProfile(), &hold, t0 + seconds(3));

        const std::string dev = tuningNotice(hold, fixedProfile(), true, params.get());
        const std::string file = tuningNotice(hold, fixedProfile(), false, params.get());
        check(dev == "TUNING IS CONTROLLED BY THE ACTIVATED PROFILE; RECEIVER AT 2115.000 MHZ",
              "device notice names the profile and the real frequency");
        check(file == "TUNING COMES FROM THE BLOCK HEADER; STREAM CENTRED AT 2115.000 MHZ",
              "file notice talks about the block header");
        check(tuningNotice(TuneHold{}, fixedProfile(), true, params.get()).empty(),
              "no notice while nothing has been refused");

        // A refused width appends its own sentence to the same band.
        TuneHold rate_only;
        const RxParams rbefore = params.get();
        RxParams r = rbefore;
        r.sample_rate = 15.36e6;
        params.set(r);
        rate_only.noteUser(rbefore, r, t0);
        followSourceMeta(params, fixedProfile(), &rate_only, t0 + seconds(3));
        const std::string width_notice = tuningNotice(rate_only, fixedProfile(), true, params.get());
        check(width_notice == "WIDTH FIXED AT 1.92 MS/S", "width-only notice");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "ALL OK", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
