// Starting the sample source is the only step before the first live frame that can wait for ever.
// The SoapySDR module opens /dev/ccci_raw_mdm and /dev/ccci_ccb_meta and asks the kernel for the
// memory window (ioctl) and maps it (mmap). With the modem out of SDR mode - normal service, an
// exception or a reset in progress - those calls can stay in the kernel, and
// DeviceSource::start() never returns.
//
// The app used to call start() on the UI thread right after drawing the boot screen, so the window
// stayed on "OPENING THE SAMPLE SOURCE..." with no event loop and no deadline: no redraw, no way
// to close it, and the launcher showing the app as starting for ever.
//
// Here start() runs in a worker thread. The caller either blocks with a deadline (headless and
// --gpu-test) or keeps the window alive through `pump` (the GUI: dispatch Wayland, redraw the boot
// screen with the elapsed time) until the source is ready or the deadline expires. On timeout the
// source is abandoned, never destroyed - its start() may still be running inside the kernel - and
// the caller falls back to the synthetic signal with a notice on screen.
#pragma once

#include "source.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace fmiq {

// Runs SampleSource::start() in a worker thread and enforces a deadline on it.
class SourceStartup {
public:
    SourceStartup(std::unique_ptr<SampleSource> src, std::chrono::milliseconds limit) {
        impl_.reset(new Impl());
        impl_->src = std::move(src);
        impl_->t0 = std::chrono::steady_clock::now();
        impl_->limit = limit.count() > 0 ? limit : std::chrono::milliseconds(6000);
        if (!impl_->src) {
            impl_->done = true;
            return;
        }
        Impl* im = impl_.get();
        SampleSource* raw = impl_->src.get();
        // The thread keeps the state alive on its own: when the deadline expires this object is
        // destroyed while start() is still running, and the worker still has to publish its result.
        std::shared_ptr<Impl> keep = impl_;
        try {
            impl_->th = std::thread([keep, im, raw] { im->run(raw); });
        } catch (const std::exception& e) {
            // No thread: report it like a failed start instead of blocking the caller.
            impl_->err = std::string("cannot create the source thread: ") + e.what();
            impl_->done = true;
        }
    }

    ~SourceStartup() {
        if (!impl_) return;
        bool done = false;
        {
            std::lock_guard<std::mutex> lk(impl_->m);
            done = impl_->done;
            // The worker may still be inside start() (an ioctl or an mmap that does not return):
            // destroying the source under it would be a use-after-free, so it is leaked on purpose.
            // This only happens once, when the deadline expires at startup.
            if (!done) impl_->src.release();
        }
        if (impl_->th.joinable()) {
            if (done) {
                impl_->th.join();
            } else {
                impl_->th.detach();
            }
        }
    }

    SourceStartup(const SourceStartup&) = delete;
    SourceStartup& operator=(const SourceStartup&) = delete;

    bool finished() const {
        std::lock_guard<std::mutex> lk(impl_->m);
        return impl_->done;
    }
    bool ok() const {
        std::lock_guard<std::mutex> lk(impl_->m);
        return impl_->done && impl_->ok;
    }
    std::string error() const {
        std::lock_guard<std::mutex> lk(impl_->m);
        return impl_->err;
    }

    double elapsedSeconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->t0).count();
    }
    // Seconds left before the deadline (0 when it has already passed).
    double remainingSeconds() const {
        const double limit = double(impl_->limit.count()) / 1000.0;
        const double left = limit - elapsedSeconds();
        return left > 0.0 ? left : 0.0;
    }
    bool expired() const { return !finished() && remainingSeconds() <= 0.0; }

    // Blocks until start() returns or the deadline passes. Returns finished().
    bool wait() {
        std::unique_lock<std::mutex> lk(impl_->m);
        impl_->cv.wait_until(lk, impl_->t0 + impl_->limit, [this] { return impl_->done; });
        return impl_->done;
    }
    // Blocks at most `budget`, never past the deadline. Returns finished().
    bool waitFor(std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lk(impl_->m);
        const auto until = std::min(impl_->t0 + impl_->limit,
                                    std::chrono::steady_clock::now() + budget);
        impl_->cv.wait_until(lk, until, [this] { return impl_->done; });
        return impl_->done;
    }

    // Ownership of the source, only after a successful start (nullptr otherwise). After a timeout
    // the source is not handed back: its start() may still be running.
    std::unique_ptr<SampleSource> take() {
        {
            std::lock_guard<std::mutex> lk(impl_->m);
            if (!impl_->done || !impl_->ok) return nullptr;
        }
        if (impl_->th.joinable()) impl_->th.join();
        std::lock_guard<std::mutex> lk(impl_->m);
        return std::move(impl_->src);
    }

private:
    struct Impl {
        std::unique_ptr<SampleSource> src;
        std::string err;
        bool ok = false;
        bool done = false;
        std::chrono::steady_clock::time_point t0;
        std::chrono::milliseconds limit{6000};
        mutable std::mutex m;
        std::condition_variable cv;
        std::thread th;

        void run(SampleSource* raw) {
            bool started = false;
            std::string e;
            try {
                started = raw->start(e);
            } catch (const std::exception& ex) {
                e = std::string("exception while starting the source: ") + ex.what();
            } catch (...) {
                e = "unknown exception while starting the source";
            }
            {
                std::lock_guard<std::mutex> lk(m);
                ok = started;
                err = e;
                done = true;
            }
            cv.notify_all();
        }
    };

    std::shared_ptr<Impl> impl_;
};

// Result of opening the source: what to run, the reason shown on screen (empty = everything is
// fine) and whether the synthetic signal replaced a device that did not answer.
struct SourcePick {
    std::unique_ptr<SampleSource> src;
    // Reason drawn in the window, short enough for the two-line notice band (see shortReason).
    // Empty = everything is fine.
    std::string notice;
    // Full reason for app.log and for the headless stderr, where there is room for it.
    std::string reason;
    bool fell_back = false;
    bool aborted = false;  // the user closed the window while it was opening
};

// Builds a source of the requested kind ("synthetic", "soapy", "device", "file").
using SourceFactory = std::function<std::unique_ptr<SampleSource>(const std::string& kind,
                                                                 std::string& err)>;

struct OpenRequest {
    std::string kind = "auto";  // auto | soapy | device | synthetic | file
    std::chrono::milliseconds limit{6000};
    // Called from the calling thread while the device is being opened. Returning false aborts the
    // wait (the window was closed). The GUI dispatches Wayland and redraws here; the headless
    // modes leave it empty and block in wait(), still bounded by `limit`.
    std::function<bool(double waited_s, double remaining_s)> pump;
};

inline std::string timeoutText(std::chrono::milliseconds limit, const std::string& kind) {
    char buf[160];
    if (kind == "soapy" || kind == "device") {
        std::snprintf(buf, sizeof(buf),
                      "the device did not answer in %.1f s opening it (SDR mode not available?)",
                      double(limit.count()) / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "opening the %s source took more than %.1f s", kind.c_str(),
                      double(limit.count()) / 1000.0);
    }
    return std::string(buf);
}

// Same reason in a few characters, for the notice band.
inline std::string timeoutBrief(std::chrono::milliseconds limit) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "no answer in %.1f s", double(limit.count()) / 1000.0);
    return std::string(buf);
}

// The notice band is two lines of the 5x7 font at scale 2: about 66 characters, less once the
// words are wrapped. A 100-character module error ran off the window and "(SYNTHETIC SIGNAL)" -
// the part that says the waterfall is not the receiver - was the first thing lost, so the reason
// is shortened here and the full text goes to app.log and to the headless stderr.
inline std::string shortReason(const std::string& s, std::size_t max_chars) {
    std::string r = s;
    const std::string prefix = "DeviceSource: ";
    if (r.compare(0, prefix.size(), prefix) == 0) r.erase(0, prefix.size());
    if (r.size() > max_chars) {
        // "cannot open /dev/ccci_raw_mdm: No such device": the tail after the last ": " is the part
        // that says what happened, so it is preferred over a cut at the beginning.
        const std::size_t colon = r.rfind(": ");
        if (colon != std::string::npos && r.size() - colon - 2 >= 8) r = r.substr(colon + 2);
    }
    if (r.size() > max_chars) {
        r.resize(max_chars);
        const std::size_t sp = r.find_last_of(' ');
        if (sp != std::string::npos && sp > max_chars / 2) r.resize(sp);
        while (!r.empty() && r.back() == ' ') r.pop_back();
        r += "...";
    }
    return r;
}

// Opens the source with a deadline. With "auto" it tries the device and falls back to the
// synthetic signal, keeping the reason in `notice` (short, for the window) and `reason` (full, for
// the log and the headless stderr); an explicit kind that fails leaves src null and the reason in
// both.
inline SourcePick openSource(const OpenRequest& req, const SourceFactory& make) {
    SourcePick pick;
    const std::chrono::milliseconds limit =
        req.limit.count() > 0 ? req.limit : std::chrono::milliseconds(6000);

    // One attempt: build, start with the deadline, pumping the UI if the caller asked for it.
    // `err` is the full reason, `brief` the one that fits in the notice band.
    const auto attempt = [&](const std::string& kind, std::string& err,
                             std::string& brief) -> std::unique_ptr<SampleSource> {
        std::string build_err;
        std::unique_ptr<SampleSource> src = make(kind, build_err);
        if (!src) {
            err = build_err.empty() ? "cannot build the " + kind + " source" : build_err;
            brief = shortReason(err, 24);
            return nullptr;
        }
        SourceStartup startup(std::move(src), limit);
        while (!startup.finished() && !startup.expired()) {
            if (req.pump) {
                if (!req.pump(startup.elapsedSeconds(), startup.remainingSeconds())) {
                    pick.aborted = true;
                    return nullptr;
                }
                startup.waitFor(std::chrono::milliseconds(5));
            } else {
                startup.wait();
            }
        }
        if (startup.finished()) {
            if (startup.ok()) return startup.take();
            err = startup.error().empty() ? std::string("could not open the source") : startup.error();
            brief = shortReason(err, 24);
            return nullptr;
        }
        err = timeoutText(limit, kind);
        brief = timeoutBrief(limit);
        return nullptr;
    };

    std::string err, brief;
    if (req.kind != "auto") {
        pick.src = attempt(req.kind, err, brief);
        if (!pick.src && !pick.aborted) {
            pick.notice = shortReason(err, 60);
            pick.reason = err;
        }
        return pick;
    }

    pick.src = attempt("soapy", err, brief);
    if (pick.src || pick.aborted) return pick;

    std::string synth_err, synth_brief;
    pick.src = attempt("synthetic", synth_err, synth_brief);
    if (pick.aborted) return pick;
    if (!pick.src) {
        pick.notice = "no source: " + shortReason(synth_err.empty() ? err : synth_err, 40);
        pick.reason = "no source: " + (synth_err.empty() ? err : synth_err);
        return pick;
    }
    pick.fell_back = true;
    pick.reason = err;
    // "NO DEVICE: " (11) + brief (<= 27) + " (SYNTHETIC SIGNAL)" (19): 57 characters, so the tag
    // stays on screen even with the word wrapping of the band.
    pick.notice = "NO DEVICE: " + (brief.empty() ? shortReason(err, 24) : brief) +
                  " (SYNTHETIC SIGNAL)";
    return pick;
}

// Test-only source: start() sleeps `ms` and then fails (ms <= 0: it never returns). It reproduces a
// module that stays in the kernel, which is what the deadline and the fallback exist for. It is
// reachable from the app with FMIQ_APP_HANG_SOURCE_MS so the headless regression can check the
// real binary without a phone.
class HangingSource final : public SampleSource {
public:
    explicit HangingSource(int ms) : ms_(ms) {}
    const char* name() const override { return "hanging-test"; }
    bool start(std::string& err) override {
        if (ms_ <= 0) {
            for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(ms_));
        err = "hanging test source: start() returned after the deadline";
        return false;
    }
    void stop() override {}
    std::size_t read(cplx*, std::size_t, int) override { return 0; }

private:
    int ms_ = 0;
};

}  // namespace fmiq
