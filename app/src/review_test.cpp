// Tests for the five fixes of the external review that can be checked without a screen.
//
// The important one is the waterfall minification: the old path built a 2048-column texture and
// let GL_LINEAR shrink it to the 456 px of the phone, which samples two texels out of every four
// or five. This test reproduces exactly that (bilinear sampling at 456 columns) with the real
// block files of tools/testdata and counts in how many rows the carrier column keeps its full
// colour, before and after collapsing the row with MAX.
#include "capture_state.hpp"
#include "pipeline.hpp"
#include "ring_state.hpp"
#include "source.hpp"
#include "waterfall.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fmiq;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_failures;
    }
}

// GL_LINEAR minification/magnification of one channel, as the GPU does it: the destination pixel
// centre is mapped to texture coordinates and the two neighbouring texels are blended.
double sample_channel(const std::uint8_t* rgb, std::size_t w, std::size_t dst_x, std::size_t dst_w,
                      int channel) {
    const double u = (double(dst_x) + 0.5) / double(dst_w);
    const double t = u * double(w) - 0.5;
    long i0 = long(std::floor(t));
    const double f = t - double(i0);
    long i1 = i0 + 1;
    i0 = std::max<long>(0, std::min<long>(long(w) - 1, i0));
    i1 = std::max<long>(0, std::min<long>(long(w) - 1, i1));
    return (1.0 - f) * double(rgb[3 * std::size_t(i0) + channel]) +
           f * double(rgb[3 * std::size_t(i1) + channel]);
}

// Red channel around the carrier column: the ramp is blue for noise and red/yellow for the
// carrier, so this separates "the tone is there" from "the tone fell between two texels".
double tone_red(const std::uint8_t* rgb, std::size_t w, std::size_t dst_col, std::size_t dst_w) {
    double best = 0.0;
    for (int d = -1; d <= 1; ++d) {
        const long c = long(dst_col) + d;
        if (c < 0 || c >= long(dst_w)) continue;
        best = std::max(best, sample_channel(rgb, w, std::size_t(c), dst_w, 0));
    }
    return best;
}

// Reads `rows` blocks of `fft` samples from a block file and returns their dB rows.
bool rows_from_file(const std::string& path, std::size_t fft, std::size_t rows,
                    std::vector<std::vector<float>>& out) {
    ParamStore params;
    FileSource src(path, params);
    std::string err;
    if (!src.start(err)) {
        std::printf("  (cannot open %s: %s)\n", path.c_str(), err.c_str());
        return false;
    }
    FftPlan plan(fft);
    plan.makeHann();
    std::vector<cplx> block(fft), spec(fft);
    out.assign(rows, std::vector<float>(fft));
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t n = src.read(block.data(), fft, 0);
        if (n < fft) return false;
        plan.process(block.data(), spec.data(), true);
        plan.magnitudesDbShifted(spec.data(), out[r].data());
    }
    return true;
}

// Portable fallback: same tone as tools/make-iqfile.py (fs/8 above the centre) with noise, so the
// test says something even when the repository data is not where it is expected.
void rows_synthetic(std::size_t fft, std::size_t rows, std::vector<std::vector<float>>& out) {
    FftPlan plan(fft);
    plan.makeHann();
    std::vector<cplx> block(fft), spec(fft);
    out.assign(rows, std::vector<float>(fft));
    std::uint32_t rng = 0x9e3779b9u;
    for (std::size_t r = 0; r < rows; ++r) {
        const double phase = double(r) * 0.37;
        for (std::size_t i = 0; i < fft; ++i) {
            const double a = 2.0 * M_PI * (double(fft) / 8.0) * double(i) / double(fft) + phase;
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            const float noise = (float(rng >> 8) * (1.0f / 8388608.0f) - 1.0f) * 0.0015f;
            block[i] = cplx(float(0.35 * std::cos(a)) + noise, float(0.35 * std::sin(a)) + noise);
        }
        plan.process(block.data(), spec.data(), true);
        plan.magnitudesDbShifted(spec.data(), out[r].data());
    }
}

struct MinifyResult {
    std::size_t full = 0;   // rows where the carrier column keeps its full colour
    std::size_t lost = 0;   // rows where it falls to the noise colour
    double mean_red = 0;
};

// Rebuilds the waterfall the way each version does it and measures the carrier column.
MinifyResult measure(const std::vector<std::vector<float>>& rows, std::size_t fft, std::size_t screen_w,
                     std::size_t row_cols) {
    MinifyResult res;
    std::vector<float> reduced;
    std::vector<std::uint8_t> rgb;
    const std::size_t tone_bin = fft / 2 + fft / 8;  // fs/8 above the centre
    const std::size_t col = std::size_t(double(tone_bin) / double(fft) * double(screen_w));
    for (const auto& db : rows) {
        const float* source = db.data();
        std::size_t n = fft;
        if (row_cols < fft) {
            reduced.resize(row_cols);
            reduce_max(db.data(), fft, reduced.data(), row_cols);
            source = reduced.data();
            n = row_cols;
        }
        rgb.assign(n * 3, 0);
        color_row(source, n, kDbMin, kDbMax, rgb.data());
        const double red = tone_red(rgb.data(), n, col, screen_w);
        res.mean_red += red;
        if (red >= 200.0) ++res.full;
        if (red < 128.0) ++res.lost;
    }
    res.mean_red /= double(rows.size() ? rows.size() : 1);
    return res;
}

void test_waterfall(const char* path) {
    const std::size_t fft = 2048, screen_w = 456, rows = 154;
    std::vector<std::vector<float>> db;
    if (!rows_from_file(path, fft, rows, db)) {
        std::printf("  %s not usable, using the synthetic tone\n", path);
        rows_synthetic(fft, rows, db);
    }
    const MinifyResult oldp = measure(db, fft, screen_w, fft);   // 2048 columns, GL_LINEAR to 456
    const MinifyResult newp = measure(db, fft, screen_w, screen_w);  // MAX per group, 456 columns
    std::printf("waterfall: %zu rows, tone bin %zu, %zux -> %zu px\n", rows, fft / 2 + fft / 8, fft,
                screen_w);
    std::printf("  before (2048 cols, GL_LINEAR): full colour in %zu/%zu rows, noise in %zu, mean red %.0f\n",
                oldp.full, rows, oldp.lost, oldp.mean_red);
    std::printf("  after  (MAX per group, %zu cols): full colour in %zu/%zu rows, noise in %zu, mean red %.0f\n",
                screen_w, newp.full, rows, newp.lost, newp.mean_red);
    check(newp.full == rows, "every waterfall row keeps the carrier column at full colour");
    check(newp.lost == 0, "no waterfall row drops the carrier to the noise colour");
    check(oldp.full < rows, "the test reproduces the old defect (carrier lost in some rows)");
}

void test_gain_shift() {
    // The GAIN control must move the whole row by the same dB and touch nothing else.
    const std::size_t fft = 2048;
    std::vector<std::vector<float>> db;
    rows_synthetic(fft, 1, db);
    const std::vector<float> reference = db[0];
    std::vector<float> shifted = reference;
    apply_db_shift(shifted.data(), shifted.size(), 28.0f);
    double worst = 0.0;
    for (std::size_t i = 0; i < fft; ++i) worst = std::max(worst, std::fabs((shifted[i] - reference[i]) - 28.0));
    std::printf("gain: 28 dB shift on the row, worst deviation %.2e dB\n", worst);
    check(worst < 1e-3, "GAIN is a pure dB shift of the finished row");
    // And it is the only sample-domain operation left: no scaling means no clipping.
    check(std::fabs(kGainReferenceDb - 12.0f) < 1e-6f, "reference gain is 12 dB");
}

void test_capture_state() {
    check(is_new_capture(true, 0, 0, 0, 0), "first block is a new capture");
    check(is_new_capture(false, 7, 6, 10, 9), "a new seq is a new capture");
    check(!is_new_capture(false, 7, 7, 10, 9), "the same seq is a repeated capture");
    check(is_new_capture(false, 0, 0, 11, 10), "block counter is the fallback when seq is always 0");
    check(!is_new_capture(false, 0, 0, 11, 11), "static block counter is not a new capture");

    const CaptureState live = capture_state(true, true, 42, 10.0, 9.98, 9.95);
    const CaptureState repeat = capture_state(true, true, 42, 10.0, 9.98, 7.00);
    const CaptureState stopped = capture_state(true, true, 42, 10.0, 5.00, 5.00);
    const CaptureState generated = capture_state(false, false, 0, 10.0, 9.98, 9.98);
    std::printf("capture: live %s | repeated %s | stopped %s | generated %s\n",
                capture_label(live).c_str(), capture_label(repeat).c_str(),
                capture_label(stopped).c_str(), capture_label(generated).c_str());
    check(live.kind == CaptureKind::Live && live.age_s < 0.1, "fresh seq is LIVE");
    check(repeat.kind == CaptureKind::Repeated && repeat.age_s > 2.9, "same seq for seconds is REPEAT");
    check(stopped.kind == CaptureKind::Stopped, "no block for seconds is NO DATA");
    check(generated.kind == CaptureKind::Generated, "a source without header is GEN");
    check(capture_label(repeat).find("REPEAT 3.0S") != std::string::npos, "REPEAT label carries the age");
    check(capture_label(live).find("#42") != std::string::npos, "label carries the sequence number");
}

void test_ring_state() {
    check(ring_state_from_setting("ok") == RingState::Ok, "rx_ring ok");
    check(ring_state_from_setting("no-device: cannot open the window (/dev/x)") == RingState::NoDevice,
          "rx_ring no-device with detail");
    check(ring_state_from_setting("no-permission") == RingState::NoPermission, "rx_ring no-permission");
    check(ring_state_from_setting("no-map: window mmap") == RingState::NoMap, "rx_ring no-map");
    check(ring_state_from_setting("no-blocks") == RingState::NoBlocks, "rx_ring no-blocks");
    check(ring_state_from_setting("no-format: 3 block(s) with signature 0xdeadbeef") == RingState::NoFormat,
          "rx_ring no-format with detail");
    check(ring_state_from_setting("v2-something") == RingState::Unknown, "an unknown value is Unknown");

    const std::string not_mapped = ring_notice(RingState::NoDevice);
    const std::string no_sweep = ring_notice(RingState::NoBlocks);
    const std::string bad_format = ring_notice(RingState::NoFormat);
    std::printf("ring: | %s | %s | %s |\n", not_mapped.c_str(), no_sweep.c_str(), bad_format.c_str());
    check(!not_mapped.empty() && !no_sweep.empty() && !bad_format.empty(), "the three causes have a message");
    check(not_mapped != no_sweep && no_sweep != bad_format && not_mapped != bad_format,
          "the three causes read differently");
    check(not_mapped.size() <= 76 && no_sweep.size() <= 76 && bad_format.size() <= 76,
          "every message fits the two-line amber band at scale 2 (38 chars per line)");
    check(ring_notice(RingState::Ok).empty(), "a working ring says nothing");
    check(ring_notice(RingState::Unknown).empty(), "a module without the setting says nothing");
    check(ring_not_mapped(RingState::NoPermission), "not-mapped groups the open failures");
    check(!ring_not_mapped(RingState::NoBlocks), "no-blocks is not a mapping failure");
}

void test_pipeline_row(const char* path) {
    // The live path end to end (acquisition thread -> ring -> FFT thread -> row ring), with the
    // same width the app uses, so the reduction is checked where it really runs.
    ParamStore params;
    std::unique_ptr<SampleSource> src(new FileSource(path, params));
    std::string err;
    if (!src->start(err)) {
        std::printf("  (cannot open %s: %s)\n", path, err.c_str());
        check(false, "pipeline test opens the block file");
        return;
    }
    Pipeline pipe(2048);
    check(pipe.setRowBins(456), "setRowBins before start is accepted");
    check(pipe.bins() == 456, "bins() is the row width handed to the UI");
    if (!pipe.start(std::move(src), err)) {
        std::printf("  (pipeline start: %s)\n", err.c_str());
        check(false, "pipeline starts");
        return;
    }
    Row row;
    bool got = false;
    for (int i = 0; i < 300 && !got; ++i) {
        got = pipe.popRow(row);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const CaptureState cap = pipe.captureState();
    pipe.stop();
    check(got, "the pipeline publishes rows");
    if (!got) return;
    check(row.bins == 456, "the published row has one column per screen pixel");
    const std::size_t tone = std::size_t(double(2048 / 2 + 2048 / 8) / 2048.0 * 456.0);
    const int red = row.rgb[3 * tone];
    std::printf("pipeline: row bins %zu, tone column %zu red %d, capture %s\n", row.bins, tone, red,
                capture_label(cap).c_str());
    check(red >= 200, "the tone column arrives at full colour from the FFT thread");
    check(cap.kind == CaptureKind::Live || cap.kind == CaptureKind::Repeated,
          "a looping recording is reported as live or repeated, never as no data");
}

}  // namespace

// The files live in tools/testdata; the test is run from app/ by the Makefile and CMake, and
// from the repository root by hand. Try both, so the documented commands all work.
std::string default_testdata(const char* name) {
    const std::string from_app = std::string("../tools/testdata/") + name;
    if (std::ifstream(from_app)) return from_app;
    const std::string from_root = std::string("tools/testdata/") + name;
    if (std::ifstream(from_root)) return from_root;
    return from_app;
}

int main(int argc, char** argv) {
    const std::string iq8_default = default_testdata("iq8-tone.bin");
    const std::string iq5_default = default_testdata("iq5-tone.bin");
    const char* iq8 = argc > 1 ? argv[1] : iq8_default.c_str();
    const char* iq5 = argc > 2 ? argv[2] : iq5_default.c_str();
    std::printf("== waterfall minification (review 1)\n");
    test_waterfall(iq8);
    test_waterfall(iq5);
    std::printf("== live pipeline row (review 1, threaded path)\n");
    test_pipeline_row(iq8);
    std::printf("== gain as a dB shift (review 5)\n");
    test_gain_shift();
    std::printf("== capture age (review 2)\n");
    test_capture_state();
    std::printf("== three ring notices (review 3)\n");
    test_ring_state();
    if (g_failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
