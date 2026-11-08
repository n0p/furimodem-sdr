#include "render_gl.hpp"

#include "font5x7.hpp"
#include "waterfall.hpp"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fmiq {
namespace {

// Peak hold on the spectrum: how fast the held maxima fall back, in dB per second. Fast enough
// that the trace is not frozen, slow enough that a narrow carrier stays visible between rows.
constexpr float kPeakDecayDbPerS = 24.0f;

const char* kSolidVs = R"(#version 300 es
layout(location=0) in vec2 a_pos;
layout(location=1) in vec4 a_col;
uniform vec2 u_res;
out vec4 v_col;
void main() {
    gl_Position = vec4(a_pos.x / u_res.x * 2.0 - 1.0, 1.0 - a_pos.y / u_res.y * 2.0, 0.0, 1.0);
    v_col = a_col;
}
)";

const char* kSolidFs = R"(#version 300 es
precision mediump float;
in vec4 v_col;
out vec4 o_col;
void main() { o_col = v_col; }
)";

const char* kTextVs = R"(#version 300 es
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_uv;
layout(location=2) in vec4 a_col;
uniform vec2 u_res;
out vec2 v_uv;
out vec4 v_col;
void main() {
    gl_Position = vec4(a_pos.x / u_res.x * 2.0 - 1.0, 1.0 - a_pos.y / u_res.y * 2.0, 0.0, 1.0);
    v_uv = a_uv;
    v_col = a_col;
}
)";

const char* kTextFs = R"(#version 300 es
precision mediump float;
uniform sampler2D u_font;
in vec2 v_uv;
in vec4 v_col;
out vec4 o_col;
void main() { o_col = vec4(v_col.rgb, v_col.a * texture(u_font, v_uv).r); }
)";

const char* kWfVs = R"(#version 300 es
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_uv;
out vec2 v_uv;
void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }
)";

// The texture is a ring: row head is the newest one. For each pixel the corresponding row is
// computed from its height, so the waterfall scrolls without moving memory on the GPU.
const char* kWfFs = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tex;
uniform float u_head;
uniform float u_rows;
in vec2 v_uv;
out vec4 o_col;
void main() {
    float row = u_head + 0.5 - v_uv.y * u_rows;
    o_col = vec4(texture(u_tex, vec2(v_uv.x, fract(row / u_rows))).rgb, 1.0);
}
)";

unsigned compile(GLenum type, const char* src, std::string& err) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        err = std::string("shader: ") + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

Painter::Painter() = default;

Painter::~Painter() {
    if (tex_) glDeleteTextures(1, &tex_);
    if (font_tex_) glDeleteTextures(1, &font_tex_);
    if (solid_vbo_) glDeleteBuffers(1, &solid_vbo_);
    if (text_vbo_) glDeleteBuffers(1, &text_vbo_);
    if (wf_vbo_) glDeleteBuffers(1, &wf_vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (solid_prog_) glDeleteProgram(solid_prog_);
    if (text_prog_) glDeleteProgram(text_prog_);
    if (wf_prog_) glDeleteProgram(wf_prog_);
}

bool Painter::buildProgram(const char* vs, const char* fs, unsigned& prog, std::string& err) {
    const unsigned v = compile(GL_VERTEX_SHADER, vs, err);
    if (!v) return false;
    const unsigned f = compile(GL_FRAGMENT_SHADER, fs, err);
    if (!f) {
        glDeleteShader(v);
        return false;
    }
    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        err = std::string("link: ") + log;
        glDeleteProgram(prog);
        prog = 0;
        return false;
    }
    return true;
}

void Painter::buildFontTexture() {
    const int cols = 16, cellw = font5x7::kAdvance, cellh = font5x7::kH + 1;
    const int w = cols * cellw, h = 6 * cellh;
    std::vector<std::uint8_t> px(std::size_t(w) * h, 0);
    for (int gi = 0; gi < font5x7::kLast - font5x7::kFirst + 1; ++gi) {
        const int cx = (gi % cols) * cellw, cy = (gi / cols) * cellh;
        for (int row = 0; row < font5x7::kH; ++row)
            for (int col = 0; col < font5x7::kW; ++col)
                if (font5x7::kGlyphs[gi][col] & (1u << row)) px[std::size_t(cy + row) * w + cx + col] = 255;
    }
    glGenTextures(1, &font_tex_);
    glBindTexture(GL_TEXTURE_2D, font_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool Painter::init(std::size_t bins, std::size_t rows, std::string& err) {
    bins_ = bins;
    rows_ = rows;
    if (!buildProgram(kSolidVs, kSolidFs, solid_prog_, err)) return false;
    if (!buildProgram(kTextVs, kTextFs, text_prog_, err)) return false;
    if (!buildProgram(kWfVs, kWfFs, wf_prog_, err)) return false;
    u_solid_res_ = glGetUniformLocation(solid_prog_, "u_res");
    u_text_res_ = glGetUniformLocation(text_prog_, "u_res");
    u_text_tex_ = glGetUniformLocation(text_prog_, "u_font");
    u_wf_tex_ = glGetUniformLocation(wf_prog_, "u_tex");
    u_wf_head_ = glGetUniformLocation(wf_prog_, "u_head");
    u_wf_rows_ = glGetUniformLocation(wf_prog_, "u_rows");

    buildFontTexture();

    glGenTextures(1, &tex_);
    setTextureColumns(bins_);

    glGenBuffers(1, &solid_vbo_);
    glGenBuffers(1, &text_vbo_);
    glGenBuffers(1, &wf_vbo_);
    glGenVertexArrays(1, &vao_);  // attributes are configured per batch
    glBindVertexArray(vao_);

    solid_.reserve(1 << 16);
    text_.reserve(1 << 14);

    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s / %s", renderer ? renderer : "?", version ? version : "?");
    gl_info_ = buf;
    return true;
}

void Painter::begin(int logical_w, int logical_h, int fb_w, int fb_h) {
    logical_w_ = logical_w;
    logical_h_ = logical_h;
    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    solid_.clear();
    text_.clear();
}

void Painter::end() {
    flushSolid();
    flushText();
}

void Painter::rect(const Rect& r, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    const float v[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x0, y1}, {x1, y0}, {x1, y1}};
    for (const auto& p : v) {
        solid_.push_back(p[0]);
        solid_.push_back(p[1]);
        solid_.push_back(c.r);
        solid_.push_back(c.g);
        solid_.push_back(c.b);
        solid_.push_back(c.a);
    }
}

void Painter::border(const Rect& r, Color c, float t) {
    rect({r.x, r.y, r.w, t}, c);
    rect({r.x, r.y + r.h - t, r.w, t}, c);
    rect({r.x, r.y + t, t, r.h - 2 * t}, c);
    rect({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c);
}

float Painter::textWidth(const char* s, float scale) const {
    std::size_t best = 0, cur = 0;
    for (const char* p = s; *p; ++p) {
        if (*p == '\n') {
            best = std::max(best, cur);
            cur = 0;
        } else {
            ++cur;
        }
    }
    best = std::max(best, cur);
    return best ? float(best) * font5x7::kAdvance * scale - scale : 0.0f;
}

void Painter::text(float x, float y, float scale, Color c, const char* s) {
    const float x0 = x;
    float cx = x, cy = y;
    const float ustep = 1.0f / 16.0f, vstep = 1.0f / 6.0f;
    const float uw = float(font5x7::kW) / (16.0f * font5x7::kAdvance);
    const float vh = float(font5x7::kH) / (6.0f * float(font5x7::kH + 1));
    for (const char* p = s; *p; ++p) {
        if (*p == '\n') {
            cx = x0;
            cy += float(font5x7::kH + 2) * scale;
            continue;
        }
        const unsigned char uc = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(*p)));
        int gi = int(uc) - font5x7::kFirst;
        if (gi < 0 || gi > font5x7::kLast - font5x7::kFirst) gi = 0;
        if (uc != ' ') {
            const int col = gi % 16, row = gi / 16;
            const float u0 = float(col) * ustep, v0 = float(row) * vstep;
            const float x1 = cx + float(font5x7::kW) * scale, y1 = cy + float(font5x7::kH) * scale;
            const float quad[6][4] = {{cx, cy, u0, v0},      {x1, cy, u0 + uw, v0},
                                      {cx, y1, u0, v0 + vh}, {cx, y1, u0, v0 + vh},
                                      {x1, cy, u0 + uw, v0}, {x1, y1, u0 + uw, v0 + vh}};
            for (const auto& q : quad) {
                text_.push_back(q[0]);
                text_.push_back(q[1]);
                text_.push_back(q[2]);
                text_.push_back(q[3]);
                text_.push_back(c.r);
                text_.push_back(c.g);
                text_.push_back(c.b);
                text_.push_back(c.a);
            }
        }
        cx += float(font5x7::kAdvance) * scale;
    }
}

void Painter::textCentered(float cx, float y, float scale, Color c, const char* s) {
    text(cx - textWidth(s, scale) * 0.5f, y, scale, c, s);
}

void Painter::textRight(float rx, float y, float scale, Color c, const char* s) {
    text(rx - textWidth(s, scale), y, scale, c, s);
}

void Painter::spectrum(const Rect& r, const float* db, std::size_t n, float db_min, float db_max, Color line,
                       Color fill) {
    if (!db || n < 2 || r.w <= 1) return;
    const auto ymap = [&](float v) {
        float t = (v - db_min) / (db_max - db_min);
        t = std::min(1.0f, std::max(0.0f, t));
        return r.y + r.h - t * r.h;
    };
    // One point per pixel column and never fewer: if the caller still hands the whole FFT row
    // (--gpu-test does), the bins are collapsed with MAX, not decimated. Drawing one of every
    // two bins is what made a narrow tone blink in and out of the curve.
    const std::size_t cols = std::max<std::size_t>(2, std::size_t(r.w));
    if (n > cols) {
        curve_.resize(cols);
        reduce_max(db, n, curve_.data(), cols);
        db = curve_.data();
        n = cols;
    }
    const float base = r.y + r.h;
    const float th = 1.1f;
    Color fill_top = fill, fill_bot = fill;
    fill_top.a *= 0.35f;
    fill_bot.a *= 0.04f;
    float px = r.x;
    float py = ymap(db[0]);
    for (std::size_t i = 1; i < n; ++i) {
        const float x = r.x + r.w * float(i) / float(n - 1);
        const float y = ymap(db[i]);
        // Fill under the curve.
        const float tri[6][2] = {{px, base}, {px, py}, {x, y}, {px, base}, {x, y}, {x, base}};
        for (int k = 0; k < 6; ++k) {
            const bool top = (k == 1 || k == 2 || k == 4);
            const Color& c = top ? fill_top : fill_bot;
            solid_.push_back(tri[k][0]);
            solid_.push_back(tri[k][1]);
            solid_.push_back(c.r);
            solid_.push_back(c.g);
            solid_.push_back(c.b);
            solid_.push_back(c.a);
        }
        // Line (fixed-thickness quad).
        const float q[6][2] = {{px, py - th}, {x, y - th}, {px, py + th},
                               {px, py + th}, {x, y - th}, {x, y + th}};
        for (int k = 0; k < 6; ++k) {
            solid_.push_back(q[k][0]);
            solid_.push_back(q[k][1]);
            solid_.push_back(line.r);
            solid_.push_back(line.g);
            solid_.push_back(line.b);
            solid_.push_back(line.a);
        }
        px = x;
        py = y;
    }

    // Peak hold: per drawn column, the maximum of the last seconds, decaying at
    // kPeakDecayDbPerS. It keeps a carrier that the current row caught weakly from vanishing
    // between rows, and it is computed on the same columns as the trace, so it cannot hide bins.
    const auto now = std::chrono::steady_clock::now();
    double dt = 0.0;
    if (peak_t_.time_since_epoch().count() != 0)
        dt = std::chrono::duration<double>(now - peak_t_).count();
    peak_t_ = now;
    if (peak_.size() != n) peak_.assign(n, -1e9f);
    const float drop = float(dt) * kPeakDecayDbPerS;
    for (std::size_t i = 0; i < n; ++i) peak_[i] = std::max(db[i], peak_[i] - drop);

    Color pk = line;
    pk.r = std::min(1.0f, line.r * 0.45f + 0.55f);
    pk.g = std::min(1.0f, line.g * 0.45f + 0.55f);
    pk.b = std::min(1.0f, line.b * 0.45f + 0.55f);
    pk.a = 0.8f;
    px = r.x;
    py = ymap(peak_[0]);
    for (std::size_t i = 1; i < n; ++i) {
        const float x = r.x + r.w * float(i) / float(n - 1);
        const float y = ymap(peak_[i]);
        const float q[6][2] = {{px, py - 0.6f}, {x, y - 0.6f}, {px, py + 0.6f},
                               {px, py + 0.6f}, {x, y - 0.6f}, {x, y + 0.6f}};
        for (int k = 0; k < 6; ++k) {
            solid_.push_back(q[k][0]);
            solid_.push_back(q[k][1]);
            solid_.push_back(pk.r);
            solid_.push_back(pk.g);
            solid_.push_back(pk.b);
            solid_.push_back(pk.a);
        }
        px = x;
        py = y;
    }
}

void Painter::setTextureColumns(std::size_t cols) {
    cols = cols ? cols : 1;
    tex_w_ = cols;
    bins_ = cols;
    glBindTexture(GL_TEXTURE_2D, tex_);
    std::vector<std::uint8_t> zero(cols * rows_ * 3, 0);
    for (std::size_t i = 0; i < cols * rows_; ++i) zero[3 * i + 2] = 128;  // map background blue
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, GLsizei(cols), GLsizei(rows_), 0, GL_RGB,
                 GL_UNSIGNED_BYTE, zero.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void Painter::pushRow(const std::uint8_t* rgb, std::size_t bins) {
    if (!rgb || bins == 0) return;
    // The pipeline decides the width of the row (the waterfall, one column per pixel). The
    // texture follows it instead of keeping a 2048-column map that would be minified 4.5:1.
    if (bins != tex_w_) setTextureColumns(bins);
    head_ = (head_ + 1) % std::uint32_t(rows_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(head_), GLsizei(bins), 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
}

void Painter::drawWaterfall(const Rect& r) {
    flushSolid();
    const float x0 = r.x / float(logical_w_) * 2.0f - 1.0f;
    const float x1 = (r.x + r.w) / float(logical_w_) * 2.0f - 1.0f;
    const float y0 = 1.0f - r.y / float(logical_h_) * 2.0f;
    const float y1 = 1.0f - (r.y + r.h) / float(logical_h_) * 2.0f;
    const float v[6][4] = {{x0, y0, 0, 0}, {x1, y0, 1, 0}, {x0, y1, 0, 1},
                           {x0, y1, 0, 1}, {x1, y0, 1, 0}, {x1, y1, 1, 1}};
    glUseProgram(wf_prog_);
    glUniform1i(u_wf_tex_, 0);
    glUniform1f(u_wf_head_, float(head_));
    glUniform1f(u_wf_rows_, float(rows_));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glBindBuffer(GL_ARRAY_BUFFER, wf_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glUseProgram(0);
}

void Painter::flushSolid() {
    if (solid_.empty()) return;
    glUseProgram(solid_prog_);
    glUniform2f(u_solid_res_, float(logical_w_), float(logical_h_));
    glBindBuffer(GL_ARRAY_BUFFER, solid_vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(solid_.size() * sizeof(float)), solid_.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(solid_.size() / 6));
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glUseProgram(0);
    solid_.clear();
}

void Painter::flushText() {
    if (text_.empty()) return;
    glUseProgram(text_prog_);
    glUniform2f(u_text_res_, float(logical_w_), float(logical_h_));
    glUniform1i(u_text_tex_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font_tex_);
    glBindBuffer(GL_ARRAY_BUFFER, text_vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(text_.size() * sizeof(float)), text_.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                          reinterpret_cast<void*>(4 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(text_.size() / 8));
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glUseProgram(0);
    text_.clear();
}

}  // namespace fmiq
