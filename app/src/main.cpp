// IQ mode demo app: tuning/band/width/gain controls and a real-time waterfall on Wayland + EGL +
// OpenGL ES 3. The source can be synthetic, a file or the device; the whole pipeline
// (acquisition -> SPSC queue -> FFT -> row) is the definitive one.
#include "pipeline.hpp"
#include "capture_state.hpp"
#include "render_egl.hpp"
#include "render_gl.hpp"
#include "ring_state.hpp"
#include "source.hpp"
#include "source_open.hpp"
#include "tune_hold.hpp"
#include "waterfall.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace fmiq;

namespace {

std::atomic<bool> g_run{true};
void onSignal(int) { g_run.store(false); }

// Launched from the app drawer there is no terminal to read stderr from (Terminal=false), so
// everything that is logged is also saved in the user state directory. It is the first place to
// look when the window does not appear.
void appLog(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "%s\n", msg);

    std::string dir;
    if (const char* st = std::getenv("XDG_STATE_HOME"); st && *st) {
        dir = st;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        dir = std::string(home) + "/.local/state";
    } else {
        return;
    }
    dir += "/furimodem-iq";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;
    const std::string path = dir + "/app.log";
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > 512u * 1024u) std::filesystem::rename(path, path + ".1", ec);
    std::FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    char ts[32] = "";
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
    if (localtime_r(&now, &tmv)) std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    std::fprintf(f, "%s %s\n", ts, msg);
    std::fclose(f);
}

struct Options {
    int headless = 0;
    int gpu_test = 0;
    std::string out;
    std::size_t fft = 2048;
    int width = 420, height = 860;
    int scale = 0;
    double seconds = 0;
    // "auto" = device if present, synthetic otherwise (and it is shown on screen). This is what
    // the launcher uses: there is no terminal to read the reason from.
    std::string source = "auto";
    std::string file;   // IQ dump for --source file
    std::string device_args;  // extra arguments for the Soapy module
    // Deadline for opening the source. The module can stay inside an ioctl/mmap while the modem
    // is not in SDR mode; the window must appear anyway (synthetic signal + notice).
    int open_timeout_ms = 6000;
    std::string notice;  // diagnostics only: draws a notice in --gpu-test
    std::string notice2; // diagnostics only: second notice line (tuning) in --gpu-test
    long check_bin = -1;  // --check-bin: requires the peak to fall in that bin
    RxParams initial;
};

void usage() {
    std::printf(
        "furimodem-iq app (IQ demo)\n"
        "  fmiq-app                         Wayland + EGL window (live waterfall)\n"
        "  fmiq-app --headless N [--out F]  N frames on the CPU -> PPM (no GPU)\n"
        "  fmiq-app --gpu-test N [--out F]  N frames in offscreen EGL -> PPM (checks shaders)\n"
        "Options:  --fft N (power of 2, 256..4096)  --band 0..5  --freq MHZ  --rate MSPS\n"
        "          --gain DB  --scale N  --seconds S  --size WxH\n"
        "          --source auto|synthetic|soapy|file   (auto = device, synthetic if missing)\n"
        "          --file PATH  --device-args ARGS      (file source / module arguments)\n"
        "          --open-timeout MS                    (deadline for opening the source,\n"
        "                                                default 6000; on timeout: synthetic)\n"
        "          --check-bin N                        (headless: requires the peak in bin N\n"
        "                                                of the centred spectrum, -fs/2..+fs/2)\n");
}

bool parse(int argc, char** argv, Options& o, std::string& err) {
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            err = std::string("missing value for ") + argv[i];
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage();
            return false;
        } else if (a == "--headless") {
            const char* v = need(i);
            if (!v) return false;
            o.headless = std::atoi(v);
        } else if (a == "--gpu-test") {
            const char* v = need(i);
            if (!v) return false;
            o.gpu_test = std::atoi(v);
        } else if (a == "--out") {
            const char* v = need(i);
            if (!v) return false;
            o.out = v;
        } else if (a == "--fft") {
            const char* v = need(i);
            if (!v) return false;
            o.fft = std::size_t(std::atoi(v));
        } else if (a == "--band") {
            const char* v = need(i);
            if (!v) return false;
            o.initial.band = std::max(0, std::min(kBandCount - 1, std::atoi(v)));
            o.initial.center_hz = kBands[o.initial.band].center_hz;
        } else if (a == "--freq") {
            const char* v = need(i);
            if (!v) return false;
            o.initial.center_hz = std::atof(v) * 1e6;
        } else if (a == "--rate") {
            const char* v = need(i);
            if (!v) return false;
            o.initial.sample_rate = std::atof(v) * 1e6;
        } else if (a == "--gain") {
            const char* v = need(i);
            if (!v) return false;
            o.initial.gain_db = float(std::atof(v));
        } else if (a == "--scale") {
            const char* v = need(i);
            if (!v) return false;
            o.scale = std::atoi(v);
        } else if (a == "--seconds") {
            const char* v = need(i);
            if (!v) return false;
            o.seconds = std::atof(v);
        } else if (a == "--source") {
            const char* v = need(i);
            if (!v) return false;
            o.source = v;
        } else if (a == "--device-args") {
            const char* v = need(i);
            if (!v) return false;
            o.device_args = v;
        } else if (a == "--open-timeout") {
            const char* v = need(i);
            if (!v) return false;
            o.open_timeout_ms = std::atoi(v);
        } else if (a == "--file") {
            const char* v = need(i);
            if (!v) return false;
            o.file = v;
        } else if (a == "--notice") {
            const char* v = need(i);
            if (!v) return false;
            o.notice = v;
        } else if (a == "--notice2") {
            const char* v = need(i);
            if (!v) return false;
            o.notice2 = v;
        } else if (a == "--check-bin") {
            const char* v = need(i);
            if (!v) return false;
            o.check_bin = std::atol(v);
        } else if (a == "--size") {
            const char* v = need(i);
            if (!v) return false;
            if (std::sscanf(v, "%dx%d", &o.width, &o.height) != 2) {
                err = "--size expects WxH";
                return false;
            }
        } else {
            err = "unknown option: " + a;
            return false;
        }
    }
    if (o.fft < 256 || o.fft > 4096 || (o.fft & (o.fft - 1)) != 0) {
        err = "--fft must be a power of 2 between 256 and 4096";
        return false;
    }
    if (o.headless < 0 || o.gpu_test < 0) {
        err = "the frame count cannot be negative";
        return false;
    }
    // A deadline under 100 ms would cut a healthy device (module load + ioctl + mmap); over 10 min
    // it is the same as no deadline at all.
    if (o.open_timeout_ms < 100 || o.open_timeout_ms > 600000) {
        err = "--open-timeout must be between 100 and 600000 ms";
        return false;
    }
    if (o.source != "auto" && o.source != "synthetic" && o.source != "soapy" && o.source != "device" &&
        o.source != "file") {
        err = "unknown source: " + o.source +
              " (auto|synthetic|soapy|file)";
        return false;
    }
    return true;
}

// Opens the source and leaves it started. With "auto" the device is tried with a deadline and, if
// it is not there or the module never answers, the synthetic signal is used: the window opens
// anyway and the reason is shown on screen and in app.log.
//
// The open never runs unbounded on this thread: SourceStartup moves start() to a worker thread and
// `pump` is called while waiting (the GUI dispatches Wayland and redraws the boot screen; the
// headless modes pass nothing and block in wait()). See source_open.hpp for why.
SourcePick pickSource(const Options& o, const ParamStore& params,
                      const std::function<bool(double, double)>& pump = {}) {
    OpenRequest req;
    req.kind = o.source;
    req.limit = std::chrono::milliseconds(o.open_timeout_ms);
    req.pump = pump;
    const SourceFactory build = [&](const std::string& kind, std::string& e) {
        // Test hook for the headless regression (app/src/source_open_test.cpp and `make test`):
        // a device source whose start() never returns, so the deadline and the fallback can be
        // exercised on a laptop with no modem. Only when the variable is set.
        if (kind == "soapy" || kind == "device") {
            const char* hang = std::getenv("FMIQ_APP_HANG_SOURCE_MS");
            if (hang && *hang) {
                return std::unique_ptr<SampleSource>(new HangingSource(std::atoi(hang)));
            }
        }
        return makeSampleSource(kind, params, o.fft, e, o.file, o.device_args);
    };
    SourcePick pick = openSource(req, build);
    if (!pick.reason.empty()) {
        appLog("source: %s", pick.reason.c_str());
    } else if (!pick.notice.empty()) {
        appLog("source: %s", pick.notice.c_str());
    }
    return pick;
}

// ---------------------------------------------------------------- UI

enum Action {
    ACT_NONE = 0,
    ACT_FREQ_M1 = 1,
    ACT_FREQ_K100_DN,
    ACT_FREQ_K100_UP,
    ACT_FREQ_P1,
    ACT_GAIN_DN = 10,
    ACT_GAIN_UP,
    ACT_BAND0 = 100,
    ACT_RATE0 = 200,
};

struct Button {
    Rect r;
    int id = 0;
    std::string label;
    bool selected = false;
};

struct UiState {
    std::vector<Button> buttons;
    Rect spectrum;
    int pressed = -1;
    bool tuning = false;
    float px = 0, py = 0;
    std::vector<float> db;
    bool have_db = false;
    std::string notice;       // persistent notice drawn under the status bar
    std::string tune_notice;  // tuning/width request the source does not follow
};

struct SceneModel {
    const float* db = nullptr;
    std::size_t bins = 0;
    int fps = 0;
    double fft_ms = 0;
    std::uint64_t dropped = 0;
    // Age and kind of the capture the picture comes from, instead of the old counter of dropped
    // blocks: it tells a live receiver from a repeated capture and from a stopped stream.
    CaptureState capture;
    const char* source = "SYNTHETIC";
    int fb_w = 0, fb_h = 0;
    // Notices drawn under the status bar: a source that fails to open or stays silent, and a
    // tuning/width request that the activated profile does not follow. They are different
    // problems and both have to be readable (launched from the drawer there is no terminal).
    std::string notice;
    std::string tune_notice;
};

struct Layout {
    Rect status, notice, spectrum, waterfall, panel, hint;
    float readout_y = 0, freq_btn_y = 0;
    float band_label_y = 0, band_btn_y = 0;
    float bw_label_y = 0, bw_btn_y = 0;
    float gain_label_y = 0, gain_btn_y = 0;
};

Layout makeLayout(int W, int H, bool has_notice = false, bool has_tune_notice = false) {
    Layout l;
    const float m = 12.0f;
    // Readability on the phone (480x1025, hands dirty in a workshop): the 5x7 font is drawn at
    // scale 2 (14 logical px) everywhere and every control is at least 44 px tall. The status bar
    // uses two rows so the source and the age of the capture are large too.
    const float status_h = 44.0f;
    const float hint_h = 26.0f;
    // One band per notice, two wrapped lines at scale 2 (2x17 px).
    const float notice_h = (has_notice ? 42.0f : 0.0f) + (has_tune_notice ? 42.0f : 0.0f);
    const float panel_h = 290.0f;
    const float spec_h = std::max(90.0f, std::min(180.0f, H * 0.19f));
    const float panel_top = float(H) - panel_h - hint_h;
    l.status = {0, 0, float(W), status_h};
    l.notice = {0, status_h, float(W), notice_h};
    l.spectrum = {m, status_h + notice_h + 8, float(W) - 2 * m, spec_h};
    l.waterfall = {m, l.spectrum.y + l.spectrum.h + 8, float(W) - 2 * m,
                   std::max(40.0f, panel_top - 8 - (l.spectrum.y + l.spectrum.h + 8))};
    l.panel = {0, panel_top, float(W), panel_h};
    l.hint = {0, float(H) - hint_h, float(W), hint_h};
    l.readout_y = panel_top + 4;
    l.freq_btn_y = panel_top + 34;
    l.band_label_y = panel_top + 86;
    l.band_btn_y = panel_top + 102;
    l.bw_label_y = panel_top + 152;
    l.bw_btn_y = panel_top + 168;
    l.gain_label_y = panel_top + 218;
    l.gain_btn_y = panel_top + 234;
    return l;
}

// Splits a notice into lines that fit the screen width. Without this, a long device error runs
// off the window exactly when it is most needed.
std::vector<std::string> wrapLines(Painter& p, const std::string& s, float maxw, std::size_t max_lines,
                                   float scale = 1.0f) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush = [&] {
        if (word.empty()) return;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (line.empty() || p.textWidth(cand.c_str(), scale) <= maxw) {
            line = cand;
        } else {
            out.push_back(line);
            line = word;
        }
        word.clear();
    };
    for (char c : s) {
        if (c == ' ' || c == '\n') flush();
        else word.push_back(c);
    }
    flush();
    if (!line.empty()) out.push_back(line);
    if (out.size() > max_lines) {
        out.resize(max_lines);
        std::string& last = out.back();
        while (!last.empty() && p.textWidth((last + "...").c_str(), scale) > maxw) last.pop_back();
        last += "...";
    }
    return out;
}

Button& addButton(UiState& st, const Rect& r, int id, std::string label, bool selected = false) {
    st.buttons.push_back(Button{r, id, std::move(label), selected});
    return st.buttons.back();
}

void drawButton(Painter& p, UiState& st, const Rect& r, int id, const std::string& label, bool selected,
                float scale = 2.0f) {
    const bool pressed = (st.pressed == id && r.contains(st.px, st.py));
    const Color fill = pressed ? hex(0x1d5c66) : (selected ? hex(0x173b45) : hex(0x141f33));
    const Color edge = selected ? hex(0x35e0d0) : hex(0x26395c);
    p.rect(r, fill);
    p.border(r, edge, 1.0f);
    const float ty = r.y + (r.h - 7.0f * scale) * 0.5f;
    p.textCentered(r.x + r.w * 0.5f, ty, scale, selected ? hex(0xd9fffb) : hex(0xc3d2e8), label.c_str());
    addButton(st, r, id, label, selected);
}

void drawScene(Painter& p, int W, int H, const RxParams& pr, const SceneModel& m, UiState& st) {
    st.buttons.clear();
    const Layout l = makeLayout(W, H, !m.notice.empty(), !m.tune_notice.empty());
    st.spectrum = l.spectrum;

    p.begin(W, H, m.fb_w > 0 ? m.fb_w : W, m.fb_h > 0 ? m.fb_h : H);
    const Color bg = hex(0x070b14);
    const Color panel = hex(0x0b1220);
    const Color spec_bg = hex(0x0c1524);
    const Color edge = hex(0x1e2c44);
    const Color dim = hex(0x647a9c);
    const Color accent = hex(0x35e0d0);

    p.rect({0, 0, float(W), float(H)}, bg);
    p.rect(l.status, panel);
    p.rect({0, l.status.h - 1, float(W), 1}, edge);

    char buf[128];
    p.text(12, 5, (W < 350 ? 1.0f : 2.0f), accent, "FURIMODEM-IQ");
    p.textRight(float(W) - 12, 5, (W < 350 ? 1.0f : 2.0f), dim, m.source);
    // Age of the last real capture, where the "D" counter used to be: green while the receiver is
    // publishing, amber when the last capture is being repeated, red when nothing arrives.
    const Color cap_col = m.capture.kind == CaptureKind::Repeated ? hex(0xffc46b)
                          : m.capture.kind == CaptureKind::Stopped ? hex(0xff7a6b)
                                                                   : hex(0x8ff2e6);
    p.text(12, 24, 2, cap_col, capture_label(m.capture).c_str());
    std::snprintf(buf, sizeof(buf), "%dFPS %.0fUS", m.fps, m.fft_ms * 1000.0);
    p.textRight(float(W) - 12, 24, 2, dim, buf);

    // Notices: the app says in the window itself why there is no device, why no samples are
    // arriving or why a control cannot move the receiver. It is the only thing visible when
    // launched from the drawer (Terminal=false).
    if (l.notice.h > 0) {
        const Color amber_bg = hex(0x2b1d0a);
        const Color amber_edge = hex(0x7a4d12);
        const Color amber_fg = hex(0xffc46b);
        p.rect(l.notice, amber_bg);
        p.rect({0, l.notice.y + l.notice.h - 1, float(W), 1}, amber_edge);
        float ny = l.notice.y + 4;
        const std::string* texts[2] = {&m.notice, &m.tune_notice};
        for (const std::string* text : texts) {
            if (text->empty()) continue;
            const auto lines = wrapLines(p, *text, float(W) - 20.0f, 2, 2.0f);
            for (std::size_t i = 0; i < lines.size(); ++i)
                p.textCentered(float(W) * 0.5f, ny + 17.0f * float(i), 2, amber_fg, lines[i].c_str());
            ny += 42.0f;
        }
    }

    // Espectro
    p.rect(l.spectrum, spec_bg);
    p.border(l.spectrum, edge, 1.0f);
    const auto ymap = [&](float db) {
        float t = (db - kDbMin) / (kDbMax - kDbMin);
        t = std::min(1.0f, std::max(0.0f, t));
        return l.spectrum.y + l.spectrum.h - t * l.spectrum.h;
    };
    const float db_lines[] = {-20.0f, -40.0f, -60.0f, -80.0f};
    for (float db : db_lines) {
        const float y = ymap(db);
        p.rect({l.spectrum.x + 1, y, l.spectrum.w - 2, 1}, hex(0x17273d));
        std::snprintf(buf, sizeof(buf), "%d", int(db));
        p.text(l.spectrum.x + 6, y - 16, 2, dim, buf);
    }
    p.rect({l.spectrum.x + l.spectrum.w * 0.5f - 0.5f, l.spectrum.y + 1, 1, l.spectrum.h - 2},
           hex(0x1d3550));
    if (m.db && m.bins > 1) p.spectrum(l.spectrum, m.db, m.bins, kDbMin, kDbMax, accent, hex(0x2fa9b8));

    // Frequency labels at the ends of the span.
    std::snprintf(buf, sizeof(buf), "%.2f", (pr.center_hz - pr.sample_rate * 0.5) / 1e6);
    p.text(l.spectrum.x + 6, l.spectrum.y + l.spectrum.h - 20, 2, dim, buf);
    std::snprintf(buf, sizeof(buf), "%.2f", (pr.center_hz + pr.sample_rate * 0.5) / 1e6);
    p.textRight(l.spectrum.x + l.spectrum.w - 6, l.spectrum.y + l.spectrum.h - 20, 2, dim, buf);

    // Waterfall
    p.drawWaterfall(l.waterfall);
    p.border(l.waterfall, edge, 1.0f);

    // Control panel
    p.rect(l.panel, panel);
    p.rect({0, l.panel.y, float(W), 1}, accent);

    std::snprintf(buf, sizeof(buf), "%.3f MHZ", pr.center_hz / 1e6);
    p.textCentered(float(W) * 0.5f, l.readout_y, 3, hex(0x8ff2e6), buf);

    const float m2 = 12.0f;
    const float gap = 6.0f;
    const float bw4 = (float(W) - 2 * m2 - 3 * gap) / 4.0f;
    const char* flabels[4] = {"-1M", "-100K", "+100K", "+1M"};
    const int fids[4] = {ACT_FREQ_M1, ACT_FREQ_K100_DN, ACT_FREQ_K100_UP, ACT_FREQ_P1};
    for (int i = 0; i < 4; ++i)
        drawButton(p, st, {m2 + i * (bw4 + gap), l.freq_btn_y, bw4, 46}, fids[i], flabels[i], false);

    p.text(m2, l.band_label_y, 2, dim, "BAND");
    std::snprintf(buf, sizeof(buf), "%.0f-%.0f MHZ", kBands[std::max(0, std::min(pr.band, kBandCount - 1))].lo_hz / 1e6,
                  kBands[std::max(0, std::min(pr.band, kBandCount - 1))].hi_hz / 1e6);
    p.textRight(float(W) - m2, l.band_label_y, 2, accent, buf);
    const float bwb = (float(W) - 2 * m2 - (kBandCount - 1) * 5.0f) / kBandCount;
    for (int i = 0; i < kBandCount; ++i)
        drawButton(p, st, {m2 + i * (bwb + 5), l.band_btn_y, bwb, 44}, ACT_BAND0 + i, kBands[i].name,
                   pr.band == i);

    p.text(m2, l.bw_label_y, 2, dim, "BW  MS/S");
    std::snprintf(buf, sizeof(buf), "%.2f", pr.sample_rate / 1e6);
    p.textRight(float(W) - m2, l.bw_label_y, 2, accent, buf);
    const float rw = (float(W) - 2 * m2 - (kSampleRateCount - 1) * gap) / kSampleRateCount;
    for (int i = 0; i < kSampleRateCount; ++i) {
        std::snprintf(buf, sizeof(buf), "%.2f", kSampleRates[i] / 1e6);
        const bool sel = std::fabs(pr.sample_rate - kSampleRates[i]) < 1.0;
        drawButton(p, st, {m2 + i * (rw + gap), l.bw_btn_y, rw, 44}, ACT_RATE0 + i, buf, sel);
    }

    p.text(m2, l.gain_label_y, 2, dim, "GAIN");
    const float gside = 76.0f;
    drawButton(p, st, {m2, l.gain_btn_y, gside, 44}, ACT_GAIN_DN, "-", false);
    std::snprintf(buf, sizeof(buf), "%d DB", int(std::lround(pr.gain_db)));
    drawButton(p, st, {m2 + gside + gap, l.gain_btn_y, float(W) - 2 * m2 - 2 * (gside + gap), 44}, ACT_NONE,
               buf, false);
    drawButton(p, st, {float(W) - m2 - gside, l.gain_btn_y, gside, 44}, ACT_GAIN_UP, "+", false);

    p.textCentered(float(W) * 0.5f, l.hint.y + 5, 2, dim,
                   "TOUCH THE SPECTRUM TO TUNE");
    p.end();
}

const char* formatLabel(IqSampleFormat f) {
    switch (f) {
        case IqSampleFormat::Packed5: return "CSD1 I/Q 5 bits";
        case IqSampleFormat::Packed8: return "CSD8 I/Q 8 bits";
        case IqSampleFormat::Cs16: return "CS16";
        default: return "?";
    }
}

// Short source label: IQ5/IQ8 when the block carries a packed format.
const char* packedTag(IqSampleFormat f) {
    switch (f) {
        case IqSampleFormat::Packed5: return " IQ5";
        case IqSampleFormat::Packed8: return " IQ8";
        default: return "";
    }
}

void applyAction(int id, ParamStore& params, TuneHold* hold) {
    if (id == ACT_NONE) return;
    const RxParams before = params.get();
    RxParams p = before;
    const BandInfo& b = kBands[std::max(0, std::min(p.band, kBandCount - 1))];
    switch (id) {
        case ACT_FREQ_M1: p.center_hz -= 1.0e6; break;
        case ACT_FREQ_K100_DN: p.center_hz -= 0.1e6; break;
        case ACT_FREQ_K100_UP: p.center_hz += 0.1e6; break;
        case ACT_FREQ_P1: p.center_hz += 1.0e6; break;
        case ACT_GAIN_DN: p.gain_db = std::max(0.0f, p.gain_db - 1.0f); break;
        case ACT_GAIN_UP: p.gain_db = std::min(45.0f, p.gain_db + 1.0f); break;
        default:
            if (id >= ACT_BAND0 && id < ACT_BAND0 + kBandCount) {
                p.band = id - ACT_BAND0;
                p.center_hz = kBands[p.band].center_hz;
            } else if (id >= ACT_RATE0 && id < ACT_RATE0 + kSampleRateCount) {
                p.sample_rate = kSampleRates[id - ACT_RATE0];
            }
            break;
    }
    if (id < ACT_BAND0)
        p.center_hz = std::max(b.lo_hz, std::min(b.hi_hz, p.center_hz));
    params.set(p);
    // The value just set must survive the next block header; the hold is what makes the button
    // do something visible instead of being reverted 20 ms later.
    if (hold) hold->noteUser(before, p);
}

void tuneAt(UiState& st, ParamStore& params, float x, TuneHold* hold) {
    if (st.spectrum.w <= 1) return;
    float t = (x - st.spectrum.x) / st.spectrum.w;
    t = std::min(1.0f, std::max(0.0f, t));
    const RxParams before = params.get();
    RxParams p = before;
    const BandInfo& b = kBands[std::max(0, std::min(p.band, kBandCount - 1))];
    double f = p.center_hz + (double(t) - 0.5) * p.sample_rate;
    f = std::max(b.lo_hz, std::min(b.hi_hz, f));
    p.center_hz = f;
    const int nb = bandForFreq(f);
    if (nb >= 0) p.band = nb;
    params.set(p);
    if (hold) hold->noteUser(before, p);
}

// ---------------------------------------------------------------- modos

int runHeadless(const Options& o) {
    if (o.headless <= 0) {
        std::fprintf(stderr, "--headless needs a frame count > 0\n");
        return 2;
    }
    ParamStore params;
    params.set(o.initial);
    const std::size_t bins = o.fft;  // full spectrum, from -fs/2 to +fs/2
    // The source is chosen as in graphical mode: that way --headless also serves to test the
    // real path (device -> FFT -> PPM) without a GPU or a display.
    SourcePick pick = pickSource(o, params);
    if (!pick.src) {
        std::fprintf(stderr, "%s\n", pick.reason.empty() ? pick.notice.c_str() : pick.reason.c_str());
        return 1;
    }
    if (!pick.notice.empty()) {
        std::fprintf(stderr, "warning: %s\n", pick.notice.c_str());
        if (!pick.reason.empty()) std::fprintf(stderr, "         %s\n", pick.reason.c_str());
    }
    SampleSource& src = *pick.src;
    auto fft = makeFft(o.fft);
    if (!fft || !fft->ok()) {
        std::fprintf(stderr, "invalid FFT\n");
        return 1;
    }
    fft->makeHann();
    std::vector<cplx> block(o.fft), spec(o.fft);
    std::vector<float> db(bins);
    Waterfall wf(bins, std::size_t(o.headless));
    std::vector<std::size_t> peak_bins;
    std::vector<float> peak_db;
    SourceMeta meta = [&] {
        SourceMeta m;
        if (!src.meta(m)) m = SourceMeta{};
        return m;
    }();
    followSourceMeta(params, meta);

    for (int frame = 0; frame < o.headless; ++frame) {
        const std::size_t n = src.read(block.data(), o.fft, 1000);
        if (n < o.fft) std::fill(block.begin() + std::ptrdiff_t(n), block.end(), cplx(0, 0));
        SourceMeta m;
        if (src.meta(m)) {
            meta = m;
            followSourceMeta(params, meta);
        }
        fft->process(block.data(), spec.data(), true);
        fft->magnitudesDbShifted(spec.data(), db.data());
        wf.push(db.data(), kDbMin, kDbMax);

        // Peak of the full spectrum. DC and its surroundings are skipped: 5-bit codes carry a
        // fixed offset and the centre bin always has energy.
        const std::size_t dc = bins / 2;
        std::size_t k = 0;
        float best = -1e9f;
        for (std::size_t i = 0; i < bins; ++i) {
            if (i + 8 >= dc && i <= dc + 8) continue;
            if (db[i] > best) {
                best = db[i];
                k = i;
            }
        }
        peak_bins.push_back(k);
        peak_db.push_back(best);
    }

    const std::string out = o.out.empty() ? "/tmp/fmiq-waterfall.ppm" : o.out;
    if (!write_ppm(out.c_str(), wf.image(), bins, std::size_t(o.headless))) {
        std::fprintf(stderr, "could not write %s\n", out.c_str());
        return 1;
    }
    std::size_t varied = 0;
    // The whole colour is compared, not just the red channel: the blue waterfall ramp leaves it
    // at 0 in almost every cell and the "no variety" warning would be false.
    for (std::size_t i = 3; i + 2 < wf.image().size(); i += 3)
        if (wf.image()[i] != wf.image()[0] || wf.image()[i + 1] != wf.image()[1] ||
            wf.image()[i + 2] != wf.image()[2]) {
            ++varied;
            break;
        }
    std::printf("headless: %d frames, FFT %zu (%s), %zux%zu -> %s, variety=%s\n", o.headless, o.fft,
                fft->name(), bins, std::size_t(o.headless), out.c_str(), varied ? "yes" : "NO");

    // Axis and peak summary: this is what the test without a phone checks.
    const RxParams pr = params.get();
    const double bin_hz = pr.sample_rate / double(o.fft);
    std::printf("  source=%s format=%s  fs=%.6f MS/s  centre=%.6f MHz  span=%.6f MHz\n",
                src.name(),
                meta.valid ? formatLabel(meta.format) : "no header",
                pr.sample_rate / 1e6, pr.center_hz / 1e6, pr.sample_rate / 1e6);

    // Most repeated bin: with a stable burst it is the tone peak.
    std::size_t mode_bin = 0;
    std::size_t mode_count = 0;
    for (std::size_t i = 0; i < peak_bins.size(); ++i) {
        std::size_t c = 0;
        for (std::size_t j = 0; j < peak_bins.size(); ++j)
            if (peak_bins[j] == peak_bins[i]) ++c;
        if (c > mode_count) {
            mode_count = c;
            mode_bin = peak_bins[i];
        }
    }
    if (!peak_bins.empty()) {
        float db_sum = 0;
        for (float v : peak_db) db_sum += v;
        const double offset_hz = (double(mode_bin) - double(bins) / 2.0) * bin_hz;
        const double peak_hz = pr.center_hz + offset_hz;
        std::printf("  peak: bin %zu in %zu/%zu frames, %+.3f kHz from centre, %.6f MHz, %.1f dBFS\n",
                    mode_bin, mode_count, peak_bins.size(), offset_hz / 1e3, peak_hz / 1e6,
                    double(db_sum / float(peak_db.size())));
        std::printf("  axis: bin 0 = %.6f MHz, bin %zu = %.6f MHz (%.3f Hz/bin)\n",
                    (pr.center_hz - pr.sample_rate * 0.5) / 1e6, bins - 1,
                    (pr.center_hz - pr.sample_rate * 0.5 + double(bins - 1) * bin_hz) / 1e6, bin_hz);
    }
    if (o.check_bin >= 0) {
        const bool ok = !peak_bins.empty() && mode_bin == std::size_t(o.check_bin);
        std::printf("  check-bin %ld: %s\n", o.check_bin, ok ? "OK" : "FAIL");
        if (!ok) return 1;
    }
    return varied ? 0 : 1;
}

int runGpuTest(const Options& o) {
    if (o.gpu_test <= 0) {
        std::fprintf(stderr, "--gpu-test needs a frame count > 0\n");
        return 2;
    }
    EglOffscreen off;
    std::string err;
    if (!off.init(o.width, o.height, err)) {
        std::fprintf(stderr, "EGL offscreen: %s\n", err.c_str());
        return 1;
    }
    Painter p;
    if (!p.init(o.fft, 256, err)) {
        std::fprintf(stderr, "GL: %s\n", err.c_str());
        return 1;
    }
    std::printf("GPU: %s\nGPU: %s\n", off.glInfo(), p.glInfo());

    ParamStore params;
    params.set(o.initial);
    // The same selection as in graphical mode: --gpu-test also draws a block file, which is how
    // the drawing is checked without a phone.
    SourcePick pick = pickSource(o, params);
    if (!pick.src) {
        std::fprintf(stderr, "%s\n", pick.reason.empty() ? pick.notice.c_str() : pick.reason.c_str());
        return 1;
    }
    if (!pick.notice.empty()) {
        std::fprintf(stderr, "warning: %s\n", pick.notice.c_str());
        if (!pick.reason.empty()) std::fprintf(stderr, "         %s\n", pick.reason.c_str());
    }
    SampleSource& src = *pick.src;
    SourceMeta meta;
    if (src.meta(meta)) followSourceMeta(params, meta);
    auto fft = makeFft(o.fft);
    fft->makeHann();
    std::vector<cplx> block(o.fft), spec(o.fft);
    std::vector<float> db(o.fft);
    // The offscreen frame must show what the live pipeline builds: one column per waterfall pixel,
    // MAX per group and the gain as a dB shift (the FFT thread does exactly this).
    const std::size_t cols = std::size_t(std::max(64, o.width - 24));
    std::vector<float> last_db(cols);
    std::vector<std::uint8_t> rgb(cols * 3);
    double fft_us = 0;
    for (int frame = 0; frame < o.gpu_test; ++frame) {
        const std::size_t n = src.read(block.data(), o.fft, 1000);
        if (n < o.fft) std::fill(block.begin() + std::ptrdiff_t(n), block.end(), cplx(0, 0));
        SourceMeta fm;
        if (src.meta(fm)) {
            meta = fm;
            followSourceMeta(params, meta);
        }
        const auto t0 = std::chrono::steady_clock::now();
        fft->process(block.data(), spec.data(), true);
        fft->magnitudesDbShifted(spec.data(), db.data());
        apply_db_shift(db.data(), db.size(), params.get().gain_db - kGainReferenceDb);
        reduce_max(db.data(), db.size(), last_db.data(), cols);
        const auto t1 = std::chrono::steady_clock::now();
        fft_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        color_row(last_db.data(), cols, kDbMin, kDbMax, rgb.data());
        p.pushRow(rgb.data(), cols);
    }

    UiState ui;
    SceneModel m;
    m.db = last_db.data();
    m.bins = last_db.size();
    m.fft_ms = fft_us / 1000.0;
    m.capture = capture_state(true, true, 8123, 10.0, 9.98, 9.95);
    m.source = "OFFSCREEN";
    // The same notice the window would show: the fallback reason included, so the offscreen PPM
    // is a faithful preview of a start without SDR mode.
    m.notice = pick.notice.empty() ? o.notice : pick.notice;
    m.tune_notice = o.notice2;
    m.fb_w = o.width;
    m.fb_h = o.height;
    drawScene(p, o.width, o.height, params.get(), m, ui);
    std::vector<std::uint8_t> img;
    if (!off.readRgb(img, err)) {
        std::fprintf(stderr, "readback: %s\n", err.c_str());
        return 1;
    }
    const std::string out = o.out.empty() ? "/tmp/fmiq-gpu.ppm" : o.out;
    if (!write_ppm(out.c_str(), img, std::size_t(o.width), std::size_t(o.height))) {
        std::fprintf(stderr, "could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("gpu-test: %d filas -> %s\n", o.gpu_test, out.c_str());
    return 0;
}

// Window with the failure reason. Launched from the drawer there is no terminal: if the app
// exits without opening a window, the launcher stays "starting" and there is nowhere to read.
int showFatal(Painter& p, EglWindow& win, const RxParams& pr, const std::string& msg) {
    win.on_close = [] { g_run.store(false); };
    win.on_key = [](int key) {
        if (key == 1 || key == 16 || key == 24) g_run.store(false);  // Esc, Q, O
    };
    const auto t0 = std::chrono::steady_clock::now();
    UiState ui;
    while (g_run.load() && !win.closed()) {
        if (!win.dispatch()) break;
        SceneModel m;
        m.source = "NO SOURCE";
        m.notice = msg;
        drawScene(p, win.width(), win.height(), pr, m, ui);
        win.swapBuffers();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > 20.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 1;
}

int runGui(const Options& o) {
    EglWindow win;
    std::string err;
    if (!win.init(o.width, o.height, o.scale, err)) {
        appLog("Wayland/EGL: %s", err.c_str());
        return 1;
    }
    Painter painter;
    if (!painter.init(o.fft, 256, err)) {
        appLog("GL: %s", err.c_str());
        return 1;
    }
    appLog("window %dx%d scale %d | GL: %s", win.width(), win.height(), win.scale(),
           painter.glInfo());

    Pipeline pipe(o.fft);
    pipe.params().set(o.initial);
    // One row column per waterfall pixel: the FFT thread reduces each row with MAX per group, so
    // the texture is not minified 4.5:1 and a narrow carrier cannot fall between two texels.
    pipe.setRowBins(std::size_t(std::max(64, win.width() - 24)));

    UiState ui;
    ui.db.resize(pipe.bins());

    // First frame before touching the source: the surface is mapped immediately and the launcher
    // stops showing the application as "starting", even if the device is slow or absent.
    const auto bootFrame = [&](const std::string& msg) {
        SceneModel boot;
        boot.source = "OPENING";
        boot.notice = msg;
        drawScene(painter, win.width(), win.height(), pipe.params().get(), boot, ui);
        win.swapBuffers();
    };
    bootFrame("OPENING THE SAMPLE SOURCE...");
    win.dispatch();  // the surface is already committed: let the compositor see it

    // While the module opens the device the window keeps being drawn and its events dispatched:
    // nothing waits for ever on this thread. The elapsed time is on screen, so a modem that is not
    // in SDR mode looks like a slow start with a countdown instead of a dead window.
    const auto pump = [&](double waited, double remaining) -> bool {
        if (!g_run.load(std::memory_order_relaxed) || win.closed()) return false;
        if (!win.dispatch()) return false;
        char msg[128];
        std::snprintf(msg, sizeof msg, "OPENING THE SAMPLE SOURCE... %.0f S (LIMIT %.0f S)", waited,
                      waited + remaining);
        bootFrame(msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        return true;
    };
    SourcePick pick = pickSource(o, pipe.params(), pump);
    if (!pick.src) {
        if (pick.aborted) return 0;  // the window was closed while the source was opening
        return showFatal(painter, win, pipe.params().get(), pick.notice);
    }
    const std::string src_name = pick.src->name();
    // State of the direct IQ ring, read once at start: "not mapped", "mapped without sweep" and
    // "unknown block format" used to arrive as the same SOAPY_SDR_TIMEOUT.
    const std::string ring_setting = pick.src->setting("rx_ring");
    const RingState ring_state = ring_state_from_setting(ring_setting);
    std::string ring_detail;
    if (const auto colon = ring_setting.find(':'); colon != std::string::npos) {
        ring_detail = ring_setting.substr(colon + 1);
        while (!ring_detail.empty() && ring_detail.front() == ' ') ring_detail.erase(0, 1);
    }
    appLog("rx_ring: %s", ring_setting.empty() ? "(not reported)" : ring_setting.c_str());
    if (!ring_detail.empty()) appLog("rx_ring detail: %s", ring_detail.c_str());
    // Holds the values set from the controls until the source has had a chance to follow them.
    TuneHold hold;
    SourceMeta initial_meta;
    const bool has_meta0 = pick.src->meta(initial_meta);
    if (has_meta0) followSourceMeta(pipe.params(), initial_meta, &hold);
    if (!pipe.start(std::move(pick.src), err)) {
        appLog("source: %s", err.c_str());
        return showFatal(painter, win, pipe.params().get(), err);
    }
    ui.notice = pick.notice;
    std::string src_label = src_name == "soapy"   ? "SOAPY"
                            : src_name == "file" ? "FILE"
                                                 : "SYNTHETIC";
    if (has_meta0 && src_name != "synthetic") src_label += packedTag(initial_meta.format);
    appLog("source: %s", src_name.c_str());

    win.on_press = [&](float x, float y) {
        ui.px = x;
        ui.py = y;
        if (ui.spectrum.contains(x, y)) {
            ui.tuning = true;
            tuneAt(ui, pipe.params(), x, &hold);
            return;
        }
        for (const auto& b : ui.buttons)
            if (b.r.contains(x, y)) {
                ui.pressed = b.id;
                return;
            }
    };
    win.on_move = [&](float x, float y) {
        ui.px = x;
        ui.py = y;
        if (ui.tuning) tuneAt(ui, pipe.params(), x, &hold);
    };
    win.on_release = [&](float x, float y) {
        ui.px = x;
        ui.py = y;
        ui.tuning = false;
        const int id = ui.pressed;
        ui.pressed = -1;
        if (id < 0) return;
        for (const auto& b : ui.buttons)
            if (b.id == id && b.r.contains(x, y)) {
                applyAction(id, pipe.params(), &hold);
                return;
            }
    };
    win.on_key = [&](int key) {
        if (key == 1 || key == 16 || key == 24) g_run.store(false);  // Esc, Q, O
    };
    win.on_close = [] { g_run.store(false); };

    Row row;
    row.db.resize(pipe.bins());
    row.rgb.resize(pipe.bins() * 3);

    int fps = 0, frames = 0;
    auto fps_t0 = std::chrono::steady_clock::now();
    auto next = std::chrono::steady_clock::now();
    const auto t_start = std::chrono::steady_clock::now();

    while (g_run.load() && !win.closed()) {
        if (!win.dispatch()) break;
        if (pipe.popRow(row)) {
            painter.pushRow(row.rgb.data(), pipe.bins());
            ui.db = row.db;
            ui.have_db = true;
        }
        // The axis follows the frequency carried in each block header, except while a value set
        // from the controls is being held (see tune_hold.hpp).
        const SourceMeta sm = pipe.sourceMeta();
        if (sm.valid) {
            followSourceMeta(pipe.params(), sm, &hold);
            const char* tag = packedTag(sm.format);
            if (src_name != "synthetic" && *tag && src_label.find(tag) == std::string::npos)
                src_label += tag;
        }
        ui.tune_notice = tuningNotice(
            hold, sm, src_name == "soapy" || src_name == "device", pipe.params().get());
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - fps_t0 >= std::chrono::seconds(1)) {
            fps = frames;
            frames = 0;
            fps_t0 = now;
        }
        const auto s = pipe.stats();
        // Device open but not a single row: the other way of staring at an empty screen with no
        // explanation. The reason comes from the ring state read at start, so "not mapped",
        // "mapped without sweep" and "unknown format" no longer read the same.
        if (!ui.have_db && src_name != "synthetic" &&
            std::chrono::duration<double>(now - t_start).count() > 3.0) {
            const std::string ring_msg = ring_notice(ring_state);
            ui.notice = ring_msg.empty() ? "NO SAMPLES FROM THE DEVICE: THE RING IS NOT PUBLISHING"
                                        : ring_msg;
        }
        SceneModel m;
        m.db = ui.have_db ? ui.db.data() : nullptr;
        m.bins = ui.db.size();
        m.fps = fps;
        m.fft_ms = s.fft_us / 1000.0;
        m.dropped = s.dropped_blocks + s.skipped_blocks + s.dropped_rows;
        m.capture = pipe.captureState();
        m.source = src_label.c_str();
        m.notice = ui.notice;
        m.tune_notice = ui.tune_notice;
        m.fb_w = win.width() * win.scale();
        m.fb_h = win.height() * win.scale();
        drawScene(painter, win.width(), win.height(), pipe.params().get(), m, ui);
        win.swapBuffers();

        next += std::chrono::microseconds(16667);
        if (next < now) next = now + std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);

        if (o.seconds > 0 && std::chrono::duration<double>(now - t_start).count() >= o.seconds)
            g_run.store(false);
    }

    const auto s = pipe.stats();
    appLog("end: %llu blocks, %llu rows, FFT %.2f ms, drops %llu",
           static_cast<unsigned long long>(s.blocks), static_cast<unsigned long long>(s.rows),
           s.fft_us / 1000.0,
           static_cast<unsigned long long>(s.dropped_blocks + s.skipped_blocks + s.dropped_rows));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Options o;
    std::string err;
    if (!parse(argc, argv, o, err)) {
        if (!err.empty()) appLog("arguments: %s", err.c_str());
        return err.empty() ? 0 : 2;
    }
    if (o.headless > 0) return runHeadless(o);
    if (o.gpu_test > 0) return runGpuTest(o);
    return runGui(o);
}
