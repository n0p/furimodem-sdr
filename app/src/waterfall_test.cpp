// Waterfall pipeline test without a GUI: synthetic signal sweeping in frequency.
#include "waterfall.hpp"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace fmiq;

int main() {
    const std::size_t n = 1024, rows = 96;
    FftPlan p(n); p.makeHann();
    std::vector<cplx> in(n), out(n);
    std::vector<float> db(n / 2);
    Waterfall wf(n / 2, rows);

    for (std::size_t r = 0; r < rows; ++r) {
        const double k0 = 60.0 + 380.0 * double(r) / double(rows);   // tone sweep
        for (std::size_t i = 0; i < n; ++i) {
            const double a = 2.0 * M_PI * k0 * double(i) / double(n);
            in[i] = cplx(float(std::cos(a)), float(std::sin(a)) * 0.3f);   // slight ellipse
        }
        p.process(in.data(), out.data(), true);
        p.magnitudesDb(out.data(), db.data());
        wf.push(db.data(), -80.0f, 0.0f);
    }

    const bool ok = write_ppm("/tmp/waterfall.ppm", wf.image(), wf.bins(), wf.rows());
    // check: the image must have colour variety (not uniform)
    std::size_t distinct = 0;
    for (std::size_t i = 3; i < wf.image().size(); i += 3)
        if (wf.image()[i] != wf.image()[0]) { ++distinct; break; }
    std::printf("waterfall %zux%zu written=%d variety=%s\n", wf.bins(), wf.rows(), int(ok),
                distinct ? "yes" : "NO");
    return ok && distinct ? 0 : 1;
}
