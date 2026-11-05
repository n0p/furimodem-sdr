// Lock-free SPSC queues for the app pipeline.
#pragma once

#include "fft.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace fmiq {

// Ring of complete sample blocks. The producer (acquisition) pushes blocks; the consumer (FFT)
// keeps the most recent one and drops the old ones if it falls behind: for a live waterfall the
// present matters, not catching up.
class BlockRing {
public:
    BlockRing(std::size_t block, std::size_t slots)
        : block_(block), slots_(slots ? slots : 1), buf_(block * slots_), dropped_(0) {}

    std::size_t blockSize() const { return block_; }

    bool push(const cplx* src) {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= slots_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        cplx* dst = buf_.data() + (h % slots_) * block_;
        for (std::size_t i = 0; i < block_; ++i) dst[i] = src[i];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Copies the most recent block to dst and marks all earlier ones as consumed.
    // skipped receives how many blocks were dropped along the way.
    bool popLatest(cplx* dst, std::size_t& skipped) {
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (h == t) return false;
        const std::uint64_t take = h - 1;
        const cplx* src = buf_.data() + (take % slots_) * block_;
        for (std::size_t i = 0; i < block_; ++i) dst[i] = src[i];
        skipped = std::size_t(h - t - 1);
        tail_.store(h, std::memory_order_release);
        return true;
    }

    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    std::size_t block_, slots_;
    std::vector<cplx> buf_;
    std::atomic<std::uint64_t> head_{0}, tail_{0}, dropped_;
};

// Processed row: spectrum in dB and RGB colours ready to upload to the texture.
struct Row {
    std::uint64_t seq = 0;
    std::size_t bins = 0;
    std::vector<float> db;
    std::vector<std::uint8_t> rgb;
};

class RowRing {
public:
    // The buffers are sized for `bins` columns; a row may carry fewer (the pipeline reduces the
    // FFT row to the width of the screen, which can change with the window size).
    RowRing(std::size_t bins, std::size_t slots) : bins_(bins), slots_(slots ? slots : 1), rows_(slots_) {
        for (auto& r : rows_) {
            r.db.resize(bins_);
            r.rgb.resize(bins_ * 3);
        }
    }

    // Producer: fills the slot and publishes. false if the ring is full (the row is dropped).
    bool beginWrite(Row*& out) { return beginWrite(out, bins_); }

    bool beginWrite(Row*& out, std::size_t bins) {
        const std::uint64_t w = head_.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (w - t >= slots_) return false;
        out = &rows_[w % slots_];
        out->bins = bins < bins_ ? bins : bins_;
        return true;
    }

    void commit() {
        const std::uint64_t w = head_.load(std::memory_order_relaxed);
        head_.store(w + 1, std::memory_order_release);
    }

    bool popLatest(Row& out, std::size_t& skipped) {
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (h == t) return false;
        const Row& src = rows_[(h - 1) % slots_];
        out.seq = src.seq;
        out.bins = src.bins;
        out.db.assign(src.db.begin(), src.db.begin() + std::ptrdiff_t(src.bins));
        out.rgb.assign(src.rgb.begin(), src.rgb.begin() + std::ptrdiff_t(src.bins * 3));
        skipped = std::size_t(h - t - 1);
        tail_.store(h, std::memory_order_release);
        return true;
    }

private:
    std::size_t bins_, slots_;
    std::vector<Row> rows_;
    std::atomic<std::uint64_t> head_{0}, tail_{0};
};

}  // namespace fmiq
