// Live pipeline: acquisition thread -> SPSC BlockRing -> FFT thread -> RowRing -> UI thread.
// The UI thread computes nothing: it takes coloured rows and uploads them to the texture.
//
// The FFT thread does the two things the review asked for and that must not run on the UI:
//   - it collapses each FFT row (2048 bins by default) to the width of the screen with MAX per
//     group (a narrow carrier is never lost when the texture is minified), and
//   - it applies the GAIN control as a dB shift on the finished row (no sample scaling, no
//     clipping spurs, and the 8-bit path -26 dB below the 5-bit one- stays visible).
#pragma once

#include "capture_state.hpp"
#include "fft_factory.hpp"
#include "ring.hpp"
#include "source.hpp"
#include "waterfall.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fmiq {

class Pipeline {
public:
    // Row width used when the app does not say otherwise. 456 is the width of the waterfall on
    // the phone (480 logical px minus the two 12 px margins), so the texture is drawn 1:1 and a
    // carrier one column wide keeps its full colour. The app can call setRowBins() with the real
    // panel width when the window is not 480 px wide.
    static constexpr std::size_t kDefaultRowBins = 456;

    explicit Pipeline(std::size_t fft_size, std::size_t row_slots = 8,
                      std::size_t row_bins = kDefaultRowBins)
        : fft_size_(fft_size),
          row_bins_(clampBins(row_bins)),
          blocks_(fft_size, 8),
          rows_(fft_size, row_slots) {}

    ~Pipeline() { stop(); }

    std::size_t fftSize() const { return fft_size_; }
    // Width of the rows handed to the UI (and of the colour columns in them).
    std::size_t bins() const { return row_bins_.load(std::memory_order_relaxed); }

    // Width of the waterfall columns. Only before start(): the UI resizes its buffers once from
    // bins() and the rows must not change size under it mid-flight.
    bool setRowBins(std::size_t n) {
        if (running_.load()) return false;
        row_bins_.store(clampBins(n), std::memory_order_relaxed);
        return true;
    }

    ParamStore& params() { return params_; }
    const ParamStore& params() const { return params_; }

    // The source must come ALREADY started (SampleSource::start): that way the app can report an
    // open failure on screen before launching the threads, and the device is not started twice.
    bool start(std::unique_ptr<SampleSource> src, std::string& err) {
        if (running_.load()) return true;
        fft_ = makeFft(fft_size_);
        if (!fft_ || !fft_->ok()) {
            err = "FFT: invalid size";
            return false;
        }
        fft_->makeHann();
        src_ = std::move(src);
        if (!src_) {
            err = "no sample source";
            return false;
        }
        t0_ = std::chrono::steady_clock::now();
        running_.store(true, std::memory_order_release);
        acq_ = std::thread(&Pipeline::acqLoop, this);
        worker_ = std::thread(&Pipeline::fftLoop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (acq_.joinable()) acq_.join();
        if (worker_.joinable()) worker_.join();
        if (src_) src_->stop();
    }

    // UI thread. Copies the most recent row; skipped counts the ones that were dropped.
    bool popRow(Row& out, std::size_t& skipped) { return rows_.popLatest(out, skipped); }
    bool popRow(Row& out) {
        std::size_t ignored = 0;
        return rows_.popLatest(out, ignored);
    }

    struct Stats {
        std::uint64_t blocks = 0;
        std::uint64_t rows = 0;
        std::uint64_t dropped_blocks = 0;
        std::uint64_t skipped_blocks = 0;
        std::uint64_t dropped_rows = 0;
        double fft_us = 0;
        std::size_t row_bins = 0;
        float gain_db_shift = 0;
    };

    Stats stats() const {
        Stats s;
        s.blocks = blocks_in_.load(std::memory_order_relaxed);
        s.rows = rows_out_.load(std::memory_order_relaxed);
        s.dropped_blocks = blocks_.dropped();
        s.skipped_blocks = skipped_blocks_.load(std::memory_order_relaxed);
        s.dropped_rows = dropped_rows_.load(std::memory_order_relaxed);
        s.fft_us = fft_us_.load(std::memory_order_relaxed);
        s.row_bins = bins();
        s.gain_db_shift = gain_shift_.load(std::memory_order_relaxed);
        return s;
    }

    // Last header read from the source (format and burst frequency). The UI uses it for the
    // axis; there is no lock because they are individual values rewritten as a whole.
    SourceMeta sourceMeta() const {
        SourceMeta m;
        m.valid = meta_valid_.load(std::memory_order_relaxed);
        m.follows_freq = meta_follows_.load(std::memory_order_relaxed);
        m.center_hz = meta_center_.load(std::memory_order_relaxed);
        m.sample_rate = meta_rate_.load(std::memory_order_relaxed);
        m.format = static_cast<IqSampleFormat>(meta_format_.load(std::memory_order_relaxed));
        m.seq = meta_seq_.load(std::memory_order_relaxed);
        m.blocks = meta_blocks_.load(std::memory_order_relaxed);
        m.stale = meta_stale_.load(std::memory_order_relaxed);
        return m;
    }

    // Age and kind of the capture the picture comes from (see capture_state.hpp). The UI draws
    // it instead of the old counter of dropped blocks.
    CaptureState captureState() const {
        const double now = running_.load() && t0_.time_since_epoch().count() != 0
                               ? std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count()
                               : 0.0;
        return capture_state(meta_valid_.load(std::memory_order_relaxed),
                             capture_has_seq_.load(std::memory_order_relaxed),
                             capture_seq_.load(std::memory_order_relaxed), now,
                             capture_last_block_.load(std::memory_order_relaxed),
                             capture_last_new_.load(std::memory_order_relaxed));
    }

    double secondsSinceStart() const {
        if (t0_.time_since_epoch().count() == 0) return 0.0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    }

private:
    static std::size_t clampBins(std::size_t n, std::size_t fft_size) {
        if (n == 0) n = fft_size;
        if (n > fft_size) n = fft_size;
        return n ? n : 1;
    }
    std::size_t clampBins(std::size_t n) const { return clampBins(n, fft_size_); }

    void publishMeta(const SourceMeta& m) {
        meta_valid_.store(m.valid, std::memory_order_relaxed);
        if (!m.valid) return;
        meta_follows_.store(m.follows_freq, std::memory_order_relaxed);
        meta_center_.store(m.center_hz, std::memory_order_relaxed);
        meta_rate_.store(m.sample_rate, std::memory_order_relaxed);
        meta_format_.store(static_cast<int>(m.format), std::memory_order_relaxed);
        meta_seq_.store(m.seq, std::memory_order_relaxed);
        meta_blocks_.store(m.blocks, std::memory_order_relaxed);
        meta_stale_.store(m.stale, std::memory_order_relaxed);
    }

    // A block was assembled: a new seq means "the receiver published this now"; the same seq
    // means the source is repeating the last capture (repeat_last, a looping recording...).
    // `blocks` counts as a heartbeat only when the header carries no sequence at all.
    void noteCapture(const SourceMeta& m, double now_s) {
        const std::uint32_t seq = m.seq;
        const std::uint32_t blocks = m.blocks;
        const bool first = !capture_has_seq_.load(std::memory_order_relaxed);
        const bool fresh = is_new_capture(first, seq, capture_seq_.load(std::memory_order_relaxed),
                                          blocks, capture_blocks_.load(std::memory_order_relaxed));
        if (fresh) capture_last_new_.store(now_s, std::memory_order_relaxed);
        capture_has_seq_.store(true, std::memory_order_relaxed);
        capture_seq_.store(seq, std::memory_order_relaxed);
        capture_blocks_.store(blocks, std::memory_order_relaxed);
        capture_last_block_.store(now_s, std::memory_order_relaxed);
    }

    void acqLoop() {
        std::vector<cplx> staging(fft_size_);
        std::size_t fill = 0;
        while (running_.load(std::memory_order_relaxed)) {
            const std::size_t n = src_->read(staging.data() + fill, fft_size_ - fill, 20);
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            SourceMeta m;
            if (src_->meta(m) && m.valid) {
                publishMeta(m);
                noteCapture(m, secondsSinceStart());
            } else {
                publishMeta(SourceMeta{});
                // No header (synthetic): the samples are generated now, so the age is zero.
                capture_last_block_.store(secondsSinceStart(), std::memory_order_relaxed);
                capture_last_new_.store(secondsSinceStart(), std::memory_order_relaxed);
            }
            fill += n;
            if (fill >= fft_size_) {
                blocks_.push(staging.data());
                blocks_in_.fetch_add(1, std::memory_order_relaxed);
                fill = 0;
            }
        }
    }

    void fftLoop() {
        std::vector<cplx> block(fft_size_), spec(fft_size_);
        std::vector<float> db(fft_size_), row_db(fft_size_);
        std::uint64_t seq = 0;
        while (running_.load(std::memory_order_relaxed)) {
            std::size_t skipped = 0;
            if (!blocks_.popLatest(block.data(), skipped)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (skipped) skipped_blocks_.fetch_add(skipped, std::memory_order_relaxed);

            const auto t0 = std::chrono::steady_clock::now();
            fft_->process(block.data(), spec.data(), true);
            fft_->magnitudesDbShifted(spec.data(), db.data());

            // GAIN as a shift on the finished row: the samples are never scaled nor clipped, so
            // raising it cannot create spurs, and the 8-bit path (26 dB weaker) can be brought
            // into the middle of the scale instead of being crushed at the bottom.
            const float gain_db = params_.get().gain_db - kGainReferenceDb;
            apply_db_shift(db.data(), fft_size_, gain_db);
            gain_shift_.store(gain_db, std::memory_order_relaxed);

            // One column per screen pixel, keeping the maximum of each group of bins.
            const std::size_t cols = std::min(row_bins_.load(std::memory_order_relaxed), fft_size_);
            reduce_max(db.data(), fft_size_, row_db.data(), cols);

            const auto t1 = std::chrono::steady_clock::now();
            const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            fft_us_.store(us, std::memory_order_relaxed);

            Row* row = nullptr;
            if (!rows_.beginWrite(row, cols)) {
                dropped_rows_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            row->seq = ++seq;
            color_row(row_db.data(), cols, kDbMin, kDbMax, row->rgb.data());
            std::copy(row_db.begin(), row_db.begin() + std::ptrdiff_t(cols), row->db.begin());
            rows_.commit();
            rows_out_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::size_t fft_size_;
    std::atomic<std::size_t> row_bins_;
    BlockRing blocks_;
    RowRing rows_;
    ParamStore params_;
    std::unique_ptr<Fft> fft_;
    std::unique_ptr<SampleSource> src_;
    std::thread acq_, worker_;
    std::chrono::steady_clock::time_point t0_{};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> blocks_in_{0}, rows_out_{0}, skipped_blocks_{0}, dropped_rows_{0};
    std::atomic<double> fft_us_{0.0};
    std::atomic<float> gain_shift_{0.0f};
    std::atomic<bool> meta_valid_{false}, meta_follows_{false};
    std::atomic<double> meta_center_{0.0}, meta_rate_{0.0};
    std::atomic<int> meta_format_{0};
    std::atomic<std::uint32_t> meta_seq_{0}, meta_blocks_{0}, meta_stale_{0};
    // Capture freshness, in seconds since the pipeline started (-1 = never).
    std::atomic<bool> capture_has_seq_{false};
    std::atomic<std::uint32_t> capture_seq_{0}, capture_blocks_{0};
    std::atomic<double> capture_last_new_{-1.0}, capture_last_block_{-1.0};
};

}  // namespace fmiq
