// Wayland window + EGL/OpenGL ES 3 context, and a windowless EGL context for GPU tests.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fmiq {

class EglWindow {
public:
    struct Impl;
    EglWindow();
    ~EglWindow();
    EglWindow(const EglWindow&) = delete;
    EglWindow& operator=(const EglWindow&) = delete;

    // scale_override = 0: infer the scale from wl_output (2 or 3 on the phone).
    bool init(int req_w, int req_h, int scale_override, std::string& err);

    bool dispatch();  // processes pending events; false if the connection was lost
    void swapBuffers();
    void close();
    bool closed() const;

    int width() const;   // logical surface coordinates
    int height() const;
    int scale() const;   // buffer_scale aplicado
    double seconds() const;

    // Input in logical coordinates. on_press/on_release allow drawing the pressed state.
    std::function<void(float, float)> on_press;
    std::function<void(float, float)> on_move;
    std::function<void(float, float)> on_release;
    std::function<void(int)> on_key;  // evdev code
    std::function<void()> on_close;

private:
    std::unique_ptr<Impl> d_;
};

// Windowless EGL context: pbuffer/FBO to verify shaders and textures on a laptop.
class EglOffscreen {
public:
    struct Impl;
    EglOffscreen();
    ~EglOffscreen();
    EglOffscreen(const EglOffscreen&) = delete;
    EglOffscreen& operator=(const EglOffscreen&) = delete;

    bool init(int w, int h, std::string& err);
    void bind();
    bool readRgb(std::vector<std::uint8_t>& rgb, std::string& err);
    int width() const;
    int height() const;
    const char* glInfo() const;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace fmiq
