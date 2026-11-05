// FFT for the waterfall: iterative radix-2, precomputed tables, no allocations in the hot path.
// Usage: include this header, create one FftPlan and call process() per block.
#pragma once
#include <complex>
#include <vector>
#include <cmath>
#include <cstddef>

namespace fmiq {

using cplx = std::complex<float>;

class FftPlan {
public:
    explicit FftPlan(std::size_t n) : n_(n) {
        // n must be a power of 2
        if (n == 0 || (n & (n - 1)) != 0) n_ = 0;
        if (!n_) return;
        log2n_ = 0;
        while ((std::size_t(1) << log2n_) < n_) ++log2n_;
        tw_.resize(n_ / 2);
        for (std::size_t i = 0; i < n_ / 2; ++i) {
            const double a = -2.0 * M_PI * double(i) / double(n_);
            tw_[i] = cplx(float(std::cos(a)), float(std::sin(a)));
        }
        rev_.resize(n_);
        for (std::size_t i = 0; i < n_; ++i) {
            std::size_t r = 0, x = i;
            for (int b = 0; b < log2n_; ++b) { r = (r << 1) | (x & 1); x >>= 1; }
            rev_[i] = r;
        }
        window_.resize(n_);
    }

    bool ok() const { return n_ != 0; }
    std::size_t size() const { return n_; }

    // Symmetric Hann window (applied in the same input pass).
    void makeHann() {
        for (std::size_t i = 0; i < n_; ++i)
            window_[i] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * double(i) / double(n_ - 1)));
    }
    void makeRect() { for (auto& w : window_) w = 1.0f; }

    // in: n samples; out: n bins in natural order (0..n/2-1, n/2..n-1). Does not allocate.
    void process(const cplx* in, cplx* out, bool applyWindow = true) {
        for (std::size_t i = 0; i < n_; ++i) {
            const cplx v = in[i];
            out[rev_[i]] = applyWindow ? cplx(v.real() * window_[i], v.imag() * window_[i]) : v;
        }
        for (std::size_t len = 2; len <= n_; len <<= 1) {
            const std::size_t half = len >> 1, step = n_ / len;
            for (std::size_t i = 0; i < n_; i += len) {
                std::size_t k = 0;
                for (std::size_t j = 0; j < half; ++j, k += step) {
                    const cplx w = tw_[k];
                    const cplx u = out[i + j];
                    const cplx t = out[i + j + half] * w;
                    out[i + j] = u + t;
                    out[i + j + half] = u - t;
                }
            }
        }
    }

    // Normalized magnitude in dB, useful half only (n/2 bins). No allocations.
    void magnitudesDb(const cplx* spec, float* db) const {
        const float norm = 2.0f / float(n_);
        for (std::size_t i = 0; i < n_ / 2; ++i) {
            const float m = std::abs(spec[i]) * norm;
            db[i] = 20.0f * std::log10(m > 1e-12f ? m : 1e-12f);
        }
    }

    // Magnitude in dB of the full spectrum in frequency order: db[0] is -fs/2 and db[n/2] the
    // centre. This is what the app draws, so a tone above the centre appears on the right.
    void magnitudesDbShifted(const cplx* spec, float* db) const {
        const float norm = 2.0f / float(n_);
        const std::size_t half = n_ / 2;
        for (std::size_t i = 0; i < n_; ++i) {
            const std::size_t k = i + half < n_ ? i + half : i + half - n_;
            const float m = std::abs(spec[k]) * norm;
            db[i] = 20.0f * std::log10(m > 1e-12f ? m : 1e-12f);
        }
    }

private:
    std::size_t n_ = 0;
    int log2n_ = 0;
    std::vector<cplx> tw_;
    std::vector<std::size_t> rev_;
    std::vector<float> window_;
};

} // namespace fmiq
