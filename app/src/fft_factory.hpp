// FFT backend selection. On the phone (aarch64/NEON) Ne10 is preferred; if it is not there, the
// built-in backend is used. The interface does not allocate in the hot path: buffers are passed
// already reserved.
#pragma once
#include "fft.hpp"          // FftFallback: implementacion propia (referencia y fallback)
#include <memory>
#include <string>
#include <cstdio>

#include <dlfcn.h>
#if defined(__aarch64__) || defined(__arm__)
#define FMIQ_HAVE_DLOPEN 1
#else
#define FMIQ_HAVE_DLOPEN 1
#endif

namespace fmiq {

enum class FftBackend { Auto, Ne10, Fallback };

// Cheap detection: tries to load the Ne10 library without linking against it.
inline bool ne10_available() {
#ifdef FMIQ_HAVE_DLOPEN
    void* h = dlopen("libNE10.so", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libNE10.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (h) { dlclose(h); return true; }
#endif
    return false;
}

// Common interface. The built-in backend already matches this signature, so it serves as the
// default implementation.
class Fft {
public:
    virtual ~Fft() = default;
    virtual bool ok() const = 0;
    virtual std::size_t size() const = 0;
    virtual void makeHann() = 0;
    virtual void makeRect() = 0;
    virtual void process(const cplx* in, cplx* out, bool applyWindow) = 0;
    virtual void magnitudesDb(const cplx* spec, float* db) const = 0;
    // Full spectrum in frequency order (-fs/2..+fs/2), which is what gets drawn.
    virtual void magnitudesDbShifted(const cplx* spec, float* db) const = 0;
    virtual const char* name() const = 0;
};

class FftFallbackImpl : public Fft {
public:
    explicit FftFallbackImpl(std::size_t n) : p_(n) {}
    bool ok() const override { return p_.ok(); }
    std::size_t size() const override { return p_.size(); }
    void makeHann() override { p_.makeHann(); }
    void makeRect() override { p_.makeRect(); }
    void process(const cplx* in, cplx* out, bool w) override { p_.process(in, out, w); }
    void magnitudesDb(const cplx* s, float* db) const override { p_.magnitudesDb(s, db); }
    void magnitudesDbShifted(const cplx* s, float* db) const override { p_.magnitudesDbShifted(s, db); }
    const char* name() const override { return "fallback"; }
private:
    FftPlan p_;
};

// Factory. In Auto: Ne10 if available, otherwise the built-in backend.
// Note: once the real Ne10 backend exists, this function will return it; for now it warns and
// uses the fallback.
inline std::unique_ptr<Fft> makeFft(std::size_t n, FftBackend pref = FftBackend::Auto) {
    const bool haveNe10 = ne10_available();
    if (pref == FftBackend::Ne10 && !haveNe10) {
        std::fprintf(stderr, "[fft] Ne10 requested but not available; using fallback\n");
    }
    if ((pref == FftBackend::Ne10 || pref == FftBackend::Auto) && haveNe10) {
        // The Ne10 wrapper is not implemented yet; the verified fallback below is used.
        std::fprintf(stderr, "[fft] Ne10 present; backend not wrapped yet, using fallback\n");
    }
    return std::make_unique<FftFallbackImpl>(n);
}

} // namespace fmiq
