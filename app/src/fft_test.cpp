// FFT verification: comparison against a direct DFT and throughput measurement.
#include "fft.hpp"
#include <cstdio>
#include <chrono>
#include <random>
#include <cmath>

using namespace fmiq;

static void dft(const cplx* in, cplx* out, std::size_t n) {
    for (std::size_t k = 0; k < n; ++k) {
        cplx acc(0, 0);
        for (std::size_t t = 0; t < n; ++t) {
            const double a = -2.0 * M_PI * double((k * t) % n) / double(n);
            acc += in[t] * cplx(float(std::cos(a)), float(std::sin(a)));
        }
        out[k] = acc;
    }
}

int main() {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> d(-1.f, 1.f);

    // 1) Correctness against a direct DFT (N=64, rectangular window)
    {
        const std::size_t n = 64;
        FftPlan p(n); p.makeRect();
        std::vector<cplx> in(n), a(n), b(n);
        for (auto& v : in) v = cplx(d(rng), d(rng));
        p.process(in.data(), a.data(), false);
        dft(in.data(), b.data(), n);
        double maxe = 0, ref = 0;
        for (std::size_t i = 0; i < n; ++i) maxe = std::max(maxe, double(std::abs(a[i] - b[i]))), ref = std::max(ref, double(std::abs(b[i])));
        printf("DFT N=64: error_max=%.3e (ref=%.1f) -> %s\n", maxe, ref, maxe < 1e-3 * ref ? "OK" : "FAIL");
    }

    // 2) Known tone: peak at the expected bin
    {
        const std::size_t n = 1024;
        FftPlan p(n); p.makeHann();
        std::vector<cplx> in(n, cplx(0, 0)), out(n);
        std::vector<float> db(n / 2);
        const int k0 = 100;
        for (std::size_t i = 0; i < n; ++i) {
            const double a = 2.0 * M_PI * double(k0) * double(i) / double(n);
            in[i] = cplx(float(std::cos(a)), float(std::sin(a)));
        }
        p.process(in.data(), out.data(), true);
        p.magnitudesDb(out.data(), db.data());
        std::size_t best = 0;
        for (std::size_t i = 1; i < n / 2; ++i) if (db[i] > db[best]) best = i;
        printf("Tone k=%d: peak at %zu (%.1f dB) -> %s\n", k0, best, db[best], best == std::size_t(k0) ? "OK" : "FAIL");
    }

    // 3) Throughput N=4096
    {
        const std::size_t n = 4096;
        FftPlan p(n); p.makeHann();
        std::vector<cplx> in(n), out(n);
        for (auto& v : in) v = cplx(d(rng), d(rng));
        const int iters = 2000;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) p.process(in.data(), out.data(), true);
        auto t1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
        printf("N=4096: %.1f us/block -> %.0f blocks/s (target >=30 fps: %s)\n",
               us, 1e6 / us, (1e6 / us) > 30 ? "OK" : "FAIL");
    }
    return 0;
}
