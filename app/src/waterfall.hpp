// Waterfall pipeline: samples -> FFT -> row in dB -> colour map -> scrolling image.
// No GPU and no dependencies: the part that can be tested on the bench.
#pragma once
#include "fft.hpp"
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace fmiq {

// Simple colour map (turbo approximated by segments): cool blue -> green -> yellow -> red.
inline void color_for_db(float db, float db_min, float db_max, uint8_t& r, uint8_t& g, uint8_t& b) {
    float t = (db - db_min) / (db_max - db_min);
    t = std::min(1.0f, std::max(0.0f, t));
    // 5 segments
    static const uint8_t stops[5][3] = {{0, 0, 128}, {0, 160, 255}, {0, 200, 64}, {255, 220, 0}, {220, 32, 32}};
    const float x = t * 4.0f;
    const int i = std::min(3, int(x));
    const float f = x - float(i);
    for (int c = 0; c < 3; ++c) {
        const float v = float(stops[i][c]) * (1.0f - f) + float(stops[i + 1][c]) * f;
        const uint8_t u = uint8_t(std::lround(v));
        (c == 0 ? r : c == 1 ? g : b) = u;
    }
}

// Collapses a row of `n` bins into `cols` columns keeping the MAXIMUM of each group.
//
// The waterfall is a 2048-bin row minified to the width of the screen (456 logical px on the
// phone). With GL_LINEAR and no mipmaps the GPU samples two texels of every four or five, so a
// narrow carrier falls between them and disappears from most rows (measured: a tone at fs/8 with
// FFT 2048 reached full intensity in 6 of 154 rows). Taking the maximum of each group before the
// texture is built removes the lottery: the group that holds the carrier always carries it, and
// minifying a run of equal values cannot dilute it.
inline void reduce_max(const float* db, std::size_t n, float* out, std::size_t cols) {
    if (!db || !out || n == 0 || cols == 0) return;
    if (cols >= n) {
        for (std::size_t i = 0; i < n; ++i) out[i] = db[i];
        return;
    }
    for (std::size_t c = 0; c < cols; ++c) {
        std::size_t a = (c * n) / cols;
        std::size_t b = ((c + 1) * n) / cols;
        if (b <= a) b = a + 1;
        float m = db[a];
        for (std::size_t i = a + 1; i < b; ++i) m = std::max(m, db[i]);
        out[c] = m;
    }
}

// Colour for a whole row already reduced (or of any length): one call per row in the FFT thread.
inline void color_row(const float* db, std::size_t n, float db_min, float db_max, std::uint8_t* rgb) {
    for (std::size_t i = 0; i < n; ++i)
        color_for_db(db[i], db_min, db_max, rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]);
}

// The GAIN control as a shift on the finished row: no sample is scaled, so nothing saturates and
// no in-band spur appears; the whole trace moves by the same number of dB, which is what makes
// the 8-bit path (26 dB below the 5-bit one) usable without crushing it against the bottom.
inline void apply_db_shift(float* db, std::size_t n, float offset_db) {
    if (!db || offset_db == 0.0f) return;
    for (std::size_t i = 0; i < n; ++i) db[i] += offset_db;
}

// Waterfall: fixed-width rows, the newest one enters at the top.
class Waterfall {
public:
    Waterfall(std::size_t bins, std::size_t rows) : bins_(bins), rows_(rows), img_(bins * rows * 3, 0) {}

    std::size_t bins() const { return bins_; }
    std::size_t rows() const { return rows_; }
    const std::vector<uint8_t>& image() const { return img_; }

    // Shifts one row up and writes the new one at the bottom. O(bins) with memmove.
    void push(const float* db, float db_min, float db_max) {
        if (rows_ > 1)
            std::memmove(img_.data(), img_.data() + bins_ * 3, bins_ * 3 * (rows_ - 1));
        uint8_t* dst = img_.data() + bins_ * 3 * (rows_ - 1);
        for (std::size_t i = 0; i < bins_; ++i) color_for_db(db[i], db_min, db_max, dst[3 * i], dst[3 * i + 1], dst[3 * i + 2]);
    }

private:
    std::size_t bins_, rows_;
    std::vector<uint8_t> img_;
};

// Dependency-free PPM (P6) writer: useful to check the pipeline without a GUI.
inline bool write_ppm(const char* path, const std::vector<uint8_t>& rgb, std::size_t w, std::size_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%zu %zu\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

} // namespace fmiq
