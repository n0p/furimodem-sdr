// OpenGL ES 3 drawing: waterfall as a circular texture updated row by row, spectrum as fill +
// line, and UI primitives (rectangles and 5x7 text) in a single batch per frame.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fmiq {

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};

inline Color hex(std::uint32_t v, float a = 1.0f) {
    return Color{float((v >> 16) & 0xff) / 255.0f, float((v >> 8) & 0xff) / 255.0f,
                 float(v & 0xff) / 255.0f, a};
}

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

class Painter {
public:
    Painter();
    ~Painter();
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    bool init(std::size_t bins, std::size_t rows, std::string& err);

    // logical_* is the UI coordinate system; fb_* is the real framebuffer size.
    void begin(int logical_w, int logical_h, int fb_w, int fb_h);
    void end();

    void rect(const Rect& r, Color c);
    void border(const Rect& r, Color c, float t = 1.0f);
    void text(float x, float y, float scale, Color c, const char* s);
    void textCentered(float cx, float y, float scale, Color c, const char* s);
    void textRight(float rx, float y, float scale, Color c, const char* s);
    float textWidth(const char* s, float scale) const;

    // Fills the area under the curve and draws the spectrum line. When `n` is larger than the
    // width in pixels the bins are collapsed with MAX (never decimated) and a peak-hold line is
    // drawn over the trace.
    void spectrum(const Rect& r, const float* db, std::size_t n, float db_min, float db_max, Color line,
                  Color fill);

    // Uploads ONE new row (RGB, bins*3 bytes) to the circular texture. The first row decides the
    // width of the texture: the pipeline already reduced it to one column per screen pixel.
    void pushRow(const std::uint8_t* rgb, std::size_t bins);
    void drawWaterfall(const Rect& r);

    std::size_t bins() const { return bins_; }
    std::size_t rows() const { return rows_; }
    std::uint32_t head() const { return head_; }
    const char* glInfo() const { return gl_info_.c_str(); }

private:
    void flushSolid();
    void flushText();
    bool buildProgram(const char* vs, const char* fs, unsigned& prog, std::string& err);
    void buildFontTexture();
    void setTextureColumns(std::size_t cols);

    std::size_t bins_ = 0, rows_ = 0, tex_w_ = 0;
    std::uint32_t head_ = 0;
    int logical_w_ = 0, logical_h_ = 0;
    unsigned tex_ = 0, font_tex_ = 0;
    unsigned solid_prog_ = 0, text_prog_ = 0, wf_prog_ = 0;
    int u_solid_res_ = -1, u_text_res_ = -1, u_text_tex_ = -1;
    int u_wf_tex_ = -1, u_wf_head_ = -1, u_wf_rows_ = -1;
    unsigned solid_vbo_ = 0, text_vbo_ = 0, wf_vbo_ = 0, vao_ = 0;
    std::vector<float> solid_;
    std::vector<float> text_;
    std::vector<float> peak_;   // peak hold, one value per drawn column
    std::vector<float> curve_;  // reduced copy of the row when it is wider than the panel
    std::chrono::steady_clock::time_point peak_t_{};
    std::string gl_info_;
};

}  // namespace fmiq
