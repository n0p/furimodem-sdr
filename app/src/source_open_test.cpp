// Regression test for the startup hang: "the SDR GUI hangs on load when the SDR mode is not
// available".
//
// The device open (SoapySDR::Device::make + setupStream + activateStream) runs on the UI thread and
// ends in the module's ioctl/mmap of /dev/ccci_raw_mdm and /dev/ccci_ccb_meta. With the modem out
// of SDR mode those calls can stay in the kernel for ever, and the window stayed on "OPENING THE
// SAMPLE SOURCE..." with no event loop. This test uses a source whose start() never returns (and
// one that takes longer than the deadline) and checks that:
//   - the deadline is enforced and respected (the call returns in about `limit`, not in 60 s),
//   - with "auto" the app ends up with the synthetic source and the on-screen notice
//     "NO DEVICE: ... (SYNTHETIC SIGNAL)",
//   - with an explicit device source it fails with a clear "did not answer" error (headless exits,
//     it does not hang),
//   - the UI pump keeps being called while waiting, and can abort (window closed),
//   - a healthy source is not cut by the deadline.
#include "source_open.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

using namespace fmiq;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A source whose start() sleeps `ms` and then succeeds: it is the "slow but working" device.
class SlowSource final : public SampleSource {
public:
    explicit SlowSource(int ms) : ms_(ms) {}
    const char* name() const override { return "slow-test"; }
    bool start(std::string&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms_));
        return true;
    }
    void stop() override {}
    std::size_t read(cplx*, std::size_t, int) override { return 0; }

private:
    int ms_;
};

// A source that fails at once with a reason (module present but no device, permission...).
class FailingSource final : public SampleSource {
public:
    const char* name() const override { return "failing-test"; }
    bool start(std::string& err) override {
        err = "cannot open /dev/ccci_raw_mdm: No such device";
        return false;
    }
    void stop() override {}
    std::size_t read(cplx*, std::size_t, int) override { return 0; }
};

const ParamStore& params() {
    static const ParamStore p;
    return p;
}

// The same factory the app uses, but the device is whatever the test needs.
SourceFactory factory(std::function<std::unique_ptr<SampleSource>()> device) {
    return [device](const std::string& kind, std::string&) -> std::unique_ptr<SampleSource> {
        if (kind == "soapy" || kind == "device") return device();
        if (kind == "synthetic") return std::unique_ptr<SampleSource>(new SyntheticSource(params(), 2048, false));
        return nullptr;
    };
}

// 1. SourceStartup itself: a start() that never returns is cut at the deadline.
void test_deadline_on_hanging_source() {
    std::printf("== the deadline cuts a start() that never returns\n");
    const auto t0 = std::chrono::steady_clock::now();
    SourceStartup st(std::unique_ptr<SampleSource>(new HangingSource(0)), std::chrono::milliseconds(150));
    const bool finished = st.wait();
    const double ms = ms_since(t0);
    std::printf("   waited %.0f ms (limit 150), finished=%d\n", ms, finished ? 1 : 0);
    check(!finished, "a source that never returns does not finish");
    check(ms >= 140.0, "the wait is not cut before the deadline");
    check(ms < 800.0, "the wait does not go past the deadline");
    check(st.expired(), "the deadline is reported as expired");
    check(!st.ok(), "a source still starting is not reported as open");
    check(st.take() == nullptr, "no source is handed back after a timeout");
    check(st.error().empty(), "there is no error text while start() is still inside the kernel");
}

// 2. A start() that takes longer than the deadline is also cut: the app cannot wait for it.
void test_deadline_on_slow_source() {
    std::printf("== a start() slower than the deadline is cut too\n");
    const auto t0 = std::chrono::steady_clock::now();
    SourceStartup st(std::unique_ptr<SampleSource>(new SlowSource(1500)), std::chrono::milliseconds(200));
    const bool finished = st.wait();
    const double ms = ms_since(t0);
    std::printf("   waited %.0f ms (limit 200)\n", ms);
    check(!finished, "a 1500 ms start() does not finish inside a 200 ms deadline");
    check(ms < 900.0, "the caller returns at the deadline, not when start() finally returns");
}

// 3. auto + device that never answers -> synthetic source and the notice shown on screen.
void test_auto_falls_back_to_synthetic_on_timeout() {
    std::printf("== auto: a device that never answers ends in the synthetic signal\n");
    OpenRequest req;
    req.kind = "auto";
    req.limit = std::chrono::milliseconds(150);
    const auto t0 = std::chrono::steady_clock::now();
    SourcePick pick = openSource(req, factory([] { return std::unique_ptr<SampleSource>(new HangingSource(0)); }));
    const double ms = ms_since(t0);
    std::printf("   %.0f ms\n   notice: %s\n   reason: %s\n", ms, pick.notice.c_str(),
                pick.reason.c_str());
    check(pick.src != nullptr, "the app has a source to run");
    check(pick.src && std::string(pick.src->name()) == "synthetic", "the source is the synthetic one");
    check(pick.fell_back, "the pick says it fell back");
    check(!pick.aborted, "the open was not aborted");
    check(pick.notice.find("NO DEVICE:") == 0, "the notice starts with NO DEVICE:");
    check(pick.notice.find("(SYNTHETIC SIGNAL)") != std::string::npos,
          "the notice says SYNTHETIC SIGNAL");
    check(pick.notice.size() <= 60, "the notice fits in the two-line band with the tag");
    check(pick.notice.find("no answer in") != std::string::npos,
          "the notice says the device did not answer");
    check(pick.reason.find("did not answer") != std::string::npos &&
              pick.reason.find("SDR mode") != std::string::npos,
          "the full reason (log, headless) explains the SDR mode hint");
    check(ms < 900.0, "the fallback happens in about the deadline");
}

// 4. Explicit --source soapy: no fallback, a clear error (this is what headless prints).
void test_explicit_device_timeout_is_an_error() {
    std::printf("== --source soapy: timeout is an error, not a hang\n");
    OpenRequest req;
    req.kind = "soapy";
    req.limit = std::chrono::milliseconds(150);
    const auto t0 = std::chrono::steady_clock::now();
    SourcePick pick = openSource(req, factory([] { return std::unique_ptr<SampleSource>(new HangingSource(0)); }));
    const double ms = ms_since(t0);
    std::printf("   %.0f ms, notice: %s\n", ms, pick.notice.c_str());
    check(pick.src == nullptr, "no source is handed back");
    check(!pick.fell_back, "an explicit source does not fall back silently");
    check(pick.notice.find("did not answer") != std::string::npos, "the reason is the timeout");
    check(ms < 900.0, "the call is bounded");
}

// 5. A fast failure keeps its own reason (no invented timeout text).
void test_auto_falls_back_on_error() {
    std::printf("== auto: a device that fails at once keeps the module's reason\n");
    OpenRequest req;
    req.kind = "auto";
    req.limit = std::chrono::milliseconds(2000);
    const auto t0 = std::chrono::steady_clock::now();
    SourcePick pick = openSource(req, factory([] { return std::unique_ptr<SampleSource>(new FailingSource()); }));
    const double ms = ms_since(t0);
    std::printf("   %.0f ms\n   notice: %s\n   reason: %s\n", ms, pick.notice.c_str(),
                pick.reason.c_str());
    check(pick.src && std::string(pick.src->name()) == "synthetic", "the synthetic source is used");
    check(pick.notice.find("No such device") != std::string::npos, "the module reason is kept on screen");
    check(pick.reason.find("/dev/ccci_raw_mdm") != std::string::npos,
          "the full reason (with the node) goes to the log");
    check(pick.notice.find("(SYNTHETIC SIGNAL)") != std::string::npos, "the tag survives on screen");
    check(ms < 900.0, "a fast failure does not wait for the deadline");
}

// 6. The GUI pump runs while waiting (the window keeps drawing) and can abort on close.
void test_pump_keeps_the_ui_alive_and_can_abort() {
    std::printf("== the pump is called while waiting and can abort\n");
    OpenRequest req;
    req.kind = "auto";
    req.limit = std::chrono::milliseconds(300);
    int calls = 0;
    double last_remaining = -1.0;
    req.pump = [&](double, double remaining) {
        ++calls;
        last_remaining = remaining;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
    };
    const auto t0 = std::chrono::steady_clock::now();
    SourcePick pick = openSource(req, factory([] { return std::unique_ptr<SampleSource>(new HangingSource(0)); }));
    const double ms = ms_since(t0);
    std::printf("   pump called %d times in %.0f ms, last remaining %.2f s\n", calls, ms, last_remaining);
    check(calls >= 5, "the UI pump is called several times while the device opens");
    check(pick.src && std::string(pick.src->name()) == "synthetic", "the fallback still happens");
    check(pick.notice.find("(SYNTHETIC SIGNAL)") != std::string::npos, "the notice is still shown");

    req.pump = [&](double, double) { return false; };  // the window was closed
    const auto t1 = std::chrono::steady_clock::now();
    SourcePick closed = openSource(req, factory([] { return std::unique_ptr<SampleSource>(new HangingSource(0)); }));
    const double ms2 = ms_since(t1);
    std::printf("   closed window: %.0f ms, aborted=%d\n", ms2, closed.aborted ? 1 : 0);
    check(closed.aborted, "closing the window aborts the open");
    check(closed.src == nullptr, "no source is started after an abort");
    check(ms2 < 150.0, "the abort does not wait for the deadline");
}

// 7. A healthy source is not cut (the deadline is a bound, not a fixed delay).
void test_healthy_source_is_not_cut() {
    std::printf("== a source that opens in time is used, with no notice\n");
    OpenRequest req;
    req.kind = "auto";
    req.limit = std::chrono::milliseconds(3000);
    const auto t0 = std::chrono::steady_clock::now();
    SourcePick pick = openSource(req, factory([] { return std::unique_ptr<SampleSource>(new SlowSource(120)); }));
    const double ms = ms_since(t0);
    std::printf("   %.0f ms, source=%s\n", ms, pick.src ? pick.src->name() : "(none)");
    check(pick.src && std::string(pick.src->name()) == "slow-test", "the device source is kept");
    check(!pick.fell_back, "there is no fallback");
    check(pick.notice.empty(), "there is no notice on screen");
    check(ms >= 100.0 && ms < 1500.0, "the wait lasts what start() needs, not the whole deadline");
    pick.src.reset();
}

// 8. The synthetic source requested explicitly goes through the same machinery untouched.
void test_explicit_synthetic() {
    std::printf("== --source synthetic still works\n");
    OpenRequest req;
    req.kind = "synthetic";
    req.limit = std::chrono::milliseconds(1000);
    SourcePick pick = openSource(req, factory([] { return std::unique_ptr<SampleSource>(new FailingSource()); }));
    check(pick.src && std::string(pick.src->name()) == "synthetic", "the synthetic source is started");
    check(pick.notice.empty(), "no notice for an explicit synthetic source");
}

}  // namespace

int main() {
    std::printf("== source open deadline (startup hang regression)\n");
    test_deadline_on_hanging_source();
    test_deadline_on_slow_source();
    test_auto_falls_back_to_synthetic_on_timeout();
    test_explicit_device_timeout_is_an_error();
    test_auto_falls_back_on_error();
    test_pump_keeps_the_ui_alive_and_can_abort();
    test_healthy_source_is_not_cut();
    test_explicit_synthetic();
    if (g_failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
