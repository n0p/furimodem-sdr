#include "render_egl.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <poll.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

namespace fmiq {

namespace {

// wl_display_roundtrip() waits for the compositor without a bound. A compositor that stops
// answering (a wedged session, a frozen greeter) leaves the app inside init(), before the first
// frame, with no window and no timeout. This is the same roundtrip - wl_display_sync plus a
// dispatch loop - with a deadline: it returns false if the answer did not arrive in time.
bool bounded_roundtrip(wl_display* display, int timeout_ms) {
    if (!display) return false;
    wl_callback* cb = wl_display_sync(display);
    if (!cb) return false;
    bool done = false;
    wl_callback_listener listener = {};
    listener.done = [](void* data, wl_callback*, std::uint32_t) {
        *static_cast<bool*>(data) = true;
    };
    wl_callback_add_listener(cb, &listener, &done);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done) {
        if (wl_display_dispatch_pending(display) < 0) break;
        if (wl_display_flush(display) < 0) break;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) break;
        pollfd p{wl_display_get_fd(display), POLLIN, 0};
        const int pr = poll(&p, 1, int(left));
        if (pr <= 0) break;  // 0 = deadline, -1 = poll error: both give up
        if (wl_display_dispatch(display) < 0) break;
    }
    wl_callback_destroy(cb);
    return done;
}

}  // namespace

struct EglWindow::Impl {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_touch* touch = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xsurf = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_egl_window* egl_window = nullptr;

    EGLDisplay edpy = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface esurf = EGL_NO_SURFACE;

    int logical_w = 420, logical_h = 860;
    int scale = 1, scale_override = 0;
    bool configured = false, closed = false;
    std::map<wl_output*, int> output_scales;
    wl_output* current_output = nullptr;
    float pointer_x = 0, pointer_y = 0;
    int32_t touch_id = -1;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    EglWindow* owner = nullptr;

    void applySize() {
        if (egl_window)
            wl_egl_window_resize(egl_window, logical_w * scale, logical_h * scale, 0, 0);
    }

    void applyScale() {
        int s = scale_override;
        if (s <= 0) {
            s = 1;
            if (current_output) {
                auto it = output_scales.find(current_output);
                if (it != output_scales.end()) s = std::max(s, it->second);
            } else {
                for (const auto& kv : output_scales) s = std::max(s, kv.second);
            }
        }
        if (s != scale) {
            scale = s;
            if (surface && wl_surface_get_version(surface) >= 3)
                wl_surface_set_buffer_scale(surface, scale);
            applySize();
        }
    }
};

// ---------------------------------------------------------------- Wayland listeners

namespace {

void wm_base_ping(void* data, xdg_wm_base* wm, std::uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

const xdg_wm_base_listener k_wm_base_listener = {wm_base_ping};

void xdg_surface_configure(void* data, xdg_surface* xs, std::uint32_t serial) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    xdg_surface_ack_configure(xs, serial);
    d->configured = true;
}

const xdg_surface_listener k_xdg_surface_listener = {xdg_surface_configure};

void toplevel_configure(void* data, xdg_toplevel*, std::int32_t w, std::int32_t h, wl_array*) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (w > 0 && h > 0) {
        d->logical_w = w;
        d->logical_h = h;
        d->applySize();
    }
}

void toplevel_close(void* data, xdg_toplevel*) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    d->closed = true;
    if (d->owner && d->owner->on_close) d->owner->on_close();
}

void toplevel_configure_bounds(void*, xdg_toplevel*, std::int32_t, std::int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

const xdg_toplevel_listener k_toplevel_listener = {toplevel_configure, toplevel_close,
                                                   toplevel_configure_bounds, toplevel_wm_capabilities};

void surface_enter(void* data, wl_surface*, wl_output* out) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    d->current_output = out;
    d->applyScale();
}

void surface_leave(void* data, wl_surface*, wl_output* out) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (d->current_output == out) d->current_output = nullptr;
    d->applyScale();
}

void surface_preferred_scale(void*, wl_surface*, std::int32_t) {}
void surface_preferred_transform(void*, wl_surface*, std::uint32_t) {}

const wl_surface_listener k_surface_listener = {surface_enter, surface_leave, surface_preferred_scale,
                                                surface_preferred_transform};

void output_scale(void* data, wl_output* out, std::int32_t factor) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (factor > 0) d->output_scales[out] = factor;
    d->applyScale();
}

// libwayland aborts if an event arrives with a NULL listener pointer; every field must be
// covered even if it is unused.
void output_geometry(void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                     std::int32_t, const char*, const char*, std::int32_t) {}
void output_mode(void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
void output_done(void*, wl_output*) {}
void output_name(void*, wl_output*, const char*) {}
void output_description(void*, wl_output*, const char*) {}

const wl_output_listener k_output_listener = {output_geometry, output_mode,   output_done,
                                              output_scale,    output_name,   output_description};

void touch_down(void* data, wl_touch*, std::uint32_t, std::uint32_t, wl_surface*, std::int32_t id,
                wl_fixed_t x, wl_fixed_t y) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (d->touch_id >= 0) return;
    d->touch_id = id;
    d->pointer_x = float(wl_fixed_to_double(x));
    d->pointer_y = float(wl_fixed_to_double(y));
    if (d->owner && d->owner->on_press)
        d->owner->on_press(d->pointer_x, d->pointer_y);
}

void touch_up(void* data, wl_touch*, std::uint32_t, std::uint32_t, std::int32_t id) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (id != d->touch_id) return;
    d->touch_id = -1;
    if (d->owner && d->owner->on_release) d->owner->on_release(d->pointer_x, d->pointer_y);
}

void touch_motion(void* data, wl_touch*, std::uint32_t, std::int32_t id, wl_fixed_t x, wl_fixed_t y) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (id != d->touch_id) return;
    d->pointer_x = float(wl_fixed_to_double(x));
    d->pointer_y = float(wl_fixed_to_double(y));
    if (d->owner && d->owner->on_move) d->owner->on_move(d->pointer_x, d->pointer_y);
}

void touch_cancel(void* data, wl_touch*) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    d->touch_id = -1;
    if (d->owner && d->owner->on_release) d->owner->on_release(d->pointer_x, d->pointer_y);
}

void touch_frame(void*, wl_touch*) {}
void touch_shape(void*, wl_touch*, std::int32_t, wl_fixed_t, wl_fixed_t) {}
void touch_orientation(void*, wl_touch*, std::int32_t, wl_fixed_t) {}

const wl_touch_listener k_touch_listener = {touch_down,   touch_up,        touch_motion, touch_frame,
                                            touch_cancel, touch_shape,     touch_orientation};

void pointer_enter(void* data, wl_pointer*, std::uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    d->pointer_x = float(wl_fixed_to_double(x));
    d->pointer_y = float(wl_fixed_to_double(y));
    if (d->owner && d->owner->on_move) d->owner->on_move(d->pointer_x, d->pointer_y);
}

void pointer_leave(void* data, wl_pointer*, std::uint32_t, wl_surface*) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (d->owner && d->owner->on_release) d->owner->on_release(d->pointer_x, d->pointer_y);
}

void pointer_motion(void* data, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    d->pointer_x = float(wl_fixed_to_double(x));
    d->pointer_y = float(wl_fixed_to_double(y));
    if (d->owner && d->owner->on_move) d->owner->on_move(d->pointer_x, d->pointer_y);
}

void pointer_button(void* data, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t button,
                    std::uint32_t state) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (button != 0x110) return;  // BTN_LEFT
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (d->owner && d->owner->on_press) d->owner->on_press(d->pointer_x, d->pointer_y);
    } else {
        if (d->owner && d->owner->on_release) d->owner->on_release(d->pointer_x, d->pointer_y);
    }
}

void pointer_axis(void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {}
void pointer_frame(void*, wl_pointer*) {}
void pointer_axis_source(void*, wl_pointer*, std::uint32_t) {}
void pointer_axis_stop(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
void pointer_axis_discrete(void*, wl_pointer*, std::uint32_t, std::int32_t) {}
[[maybe_unused]] void pointer_axis_value120(void*, wl_pointer*, std::uint32_t, std::int32_t) {}
[[maybe_unused]] void pointer_axis_relative_direction(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
[[maybe_unused]] void pointer_warp(void*, wl_pointer*, wl_fixed_t, wl_fixed_t) {}

// Filled in by name (not by position) so it compiles with any Wayland version: the new fields
// of recent versions do not exist in the old ones.
const wl_pointer_listener& pointer_listener() {
    static wl_pointer_listener l{};
    static const bool once = [] {
        l.enter = pointer_enter;
        l.leave = pointer_leave;
        l.motion = pointer_motion;
        l.button = pointer_button;
        l.axis = pointer_axis;
        l.frame = pointer_frame;
        l.axis_source = pointer_axis_source;
        l.axis_stop = pointer_axis_stop;
        l.axis_discrete = pointer_axis_discrete;
        return true;
    }();
    (void)once;
    return l;
}

void keyboard_keymap(void* data, wl_keyboard*, std::uint32_t, std::int32_t fd, std::uint32_t) {
    (void)data;
    if (fd >= 0) ::close(fd);
}

void keyboard_key(void* data, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t key,
                  std::uint32_t state) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && d->owner && d->owner->on_key)
        d->owner->on_key(int(key));
}

void keyboard_enter(void*, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*) {}
void keyboard_leave(void*, wl_keyboard*, std::uint32_t, wl_surface*) {}
void keyboard_modifiers(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::uint32_t) {}
void keyboard_repeat_info(void*, wl_keyboard*, std::int32_t, std::int32_t) {}

const wl_keyboard_listener k_keyboard_listener = {keyboard_keymap,    keyboard_enter,
                                                  keyboard_leave,     keyboard_key,
                                                  keyboard_modifiers, keyboard_repeat_info};

void seat_capabilities(void* data, wl_seat* seat, std::uint32_t caps) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(d->pointer, &pointer_listener(), d);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
        d->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(d->touch, &k_touch_listener, d);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
        d->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(d->keyboard, &k_keyboard_listener, d);
    }
}

void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener k_seat_listener = {seat_capabilities, seat_name};

void registry_global(void* data, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t version) {
    auto* d = static_cast<EglWindow::Impl*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        d->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        d->wm_base =
            static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(d->wm_base, &k_wm_base_listener, d);
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        d->seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(d->seat, &k_seat_listener, d);
    } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        auto* out = static_cast<wl_output*>(
            wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 2u)));
        wl_output_add_listener(out, &k_output_listener, d);
    }
}

void registry_global_remove(void*, wl_registry*, std::uint32_t) {}

const wl_registry_listener k_registry_listener = {registry_global, registry_global_remove};

}  // namespace

// ---------------------------------------------------------------- EglWindow

EglWindow::EglWindow() : d_(std::make_unique<Impl>()) {}

EglWindow::~EglWindow() {
    Impl& d = *d_;
    if (d.edpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(d.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (d.esurf != EGL_NO_SURFACE) eglDestroySurface(d.edpy, d.esurf);
        if (d.ctx != EGL_NO_CONTEXT) eglDestroyContext(d.edpy, d.ctx);
        eglTerminate(d.edpy);
    }
    if (d.egl_window) wl_egl_window_destroy(d.egl_window);
    if (d.toplevel) xdg_toplevel_destroy(d.toplevel);
    if (d.xsurf) xdg_surface_destroy(d.xsurf);
    if (d.surface) wl_surface_destroy(d.surface);
    if (d.touch) wl_touch_destroy(d.touch);
    if (d.pointer) wl_pointer_destroy(d.pointer);
    if (d.keyboard) wl_keyboard_destroy(d.keyboard);
    if (d.seat) wl_seat_destroy(d.seat);
    if (d.wm_base) xdg_wm_base_destroy(d.wm_base);
    if (d.compositor) wl_compositor_destroy(d.compositor);
    if (d.registry) wl_registry_destroy(d.registry);
    if (d.display) wl_display_disconnect(d.display);
}

bool EglWindow::init(int req_w, int req_h, int scale_override, std::string& err) {
    Impl& d = *d_;
    d.owner = this;
    d.logical_w = req_w;
    d.logical_h = req_h;
    d.scale_override = scale_override;
    d.t0 = std::chrono::steady_clock::now();

    d.display = wl_display_connect(nullptr);
    if (!d.display) {
        err = "cannot connect to the Wayland compositor (WAYLAND_DISPLAY)";
        return false;
    }
    d.registry = wl_display_get_registry(d.display);
    wl_registry_add_listener(d.registry, &k_registry_listener, &d);
    // Bounded: a compositor that does not answer must fail here with a reason, not leave the app
    // blocked before its first frame (see bounded_roundtrip).
    if (!bounded_roundtrip(d.display, 3000)) {  // globals
        err = "the compositor did not answer in 3 s (wl_display_roundtrip)";
        return false;
    }
    if (!bounded_roundtrip(d.display, 3000)) {  // output scale
        std::fprintf(stderr, "[egl] no answer to the output roundtrip; scale stays %d\n", d.scale);
    }

    if (!d.compositor || !d.wm_base) {
        err = "the compositor does not expose wl_compositor/xdg_wm_base";
        return false;
    }
    d.applyScale();  // initial scale from the outputs

    d.surface = wl_compositor_create_surface(d.compositor);
    wl_surface_add_listener(d.surface, &k_surface_listener, &d);
    if (wl_surface_get_version(d.surface) >= 3) wl_surface_set_buffer_scale(d.surface, d.scale);
    d.xsurf = xdg_wm_base_get_xdg_surface(d.wm_base, d.surface);
    xdg_surface_add_listener(d.xsurf, &k_xdg_surface_listener, &d);
    d.toplevel = xdg_surface_get_toplevel(d.xsurf);
    xdg_toplevel_add_listener(d.toplevel, &k_toplevel_listener, &d);
    xdg_toplevel_set_title(d.toplevel, "furimodem-iq");
    // The app_id must match the .desktop file name (fmiq-app.desktop): otherwise the launcher
    // does not recognise the window as its own and leaves the application "starting" forever.
    xdg_toplevel_set_app_id(d.toplevel, "fmiq-app");
    xdg_toplevel_set_min_size(d.toplevel, 320, 480);
    wl_surface_commit(d.surface);
    // The configure arrives from the compositor: wait for it, but not for ever (the first frame is
    // drawn right after this and the app must not stay inside init()). A missing configure is not
    // fatal: the surface is already committed and the frame will be shown when it arrives.
    if (!bounded_roundtrip(d.display, 3000)) {
        std::fprintf(stderr, "[egl] no initial configure in 3 s; drawing anyway\n");
    }

    auto getPlatformDisplay =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    bool egl_ok = false;
    if (getPlatformDisplay) {
        d.edpy = getPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, d.display, nullptr);
        if (d.edpy != EGL_NO_DISPLAY) egl_ok = eglInitialize(d.edpy, nullptr, nullptr);
    }
    if (!egl_ok) {
        // Some EGL implementations (libhybris) reject the explicit platform: retry the classic way.
        d.edpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(d.display));
        egl_ok = d.edpy != EGL_NO_DISPLAY && eglInitialize(d.edpy, nullptr, nullptr);
    }
    if (!egl_ok) {
        err = "eglInitialize failed";
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        err = "eglBindAPI(EGL_OPENGL_ES_API) failed";
        return false;
    }

    EGLint cfg_attr[] = {EGL_SURFACE_TYPE,   EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_ES3_BIT, EGL_RED_SIZE,   8,
                         EGL_GREEN_SIZE,     8,              EGL_BLUE_SIZE,
                         8,                  EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(d.edpy, cfg_attr, &d.config, 1, &n) || n == 0) {
        cfg_attr[3] = EGL_OPENGL_ES2_BIT;
        if (!eglChooseConfig(d.edpy, cfg_attr, &d.config, 1, &n) || n == 0) {
            err = "no EGL RGB888 configuration for OpenGL ES";
            return false;
        }
    }
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    d.ctx = eglCreateContext(d.edpy, d.config, EGL_NO_CONTEXT, ctx_attr);
    if (d.ctx == EGL_NO_CONTEXT) {
        err = "eglCreateContext(ES3) failed";
        return false;
    }

    d.egl_window = wl_egl_window_create(d.surface, d.logical_w * d.scale, d.logical_h * d.scale);
    d.esurf = eglCreateWindowSurface(d.edpy, d.config,
                                     reinterpret_cast<EGLNativeWindowType>(d.egl_window), nullptr);
    if (d.esurf == EGL_NO_SURFACE) {
        err = "eglCreateWindowSurface failed";
        return false;
    }
    if (!eglMakeCurrent(d.edpy, d.esurf, d.esurf, d.ctx)) {
        err = "eglMakeCurrent failed";
        return false;
    }
    eglSwapInterval(d.edpy, 1);
    return true;
}

bool EglWindow::dispatch() {
    Impl& d = *d_;
    if (!d.display || d.closed) return !d.closed;
    if (wl_display_dispatch_pending(d.display) < 0) {
        d.closed = true;
        return false;
    }
    wl_display_flush(d.display);
    pollfd p{d.display ? wl_display_get_fd(d.display) : -1, POLLIN, 0};
    if (p.fd >= 0 && poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        if (wl_display_dispatch(d.display) < 0) {
            d.closed = true;
            return false;
        }
    }
    return !d.closed;
}

void EglWindow::swapBuffers() {
    Impl& d = *d_;
    if (d.edpy != EGL_NO_DISPLAY && d.esurf != EGL_NO_SURFACE) {
        if (!eglSwapBuffers(d.edpy, d.esurf)) {
            std::fprintf(stderr, "[egl] eglSwapBuffers failed: 0x%x\n", eglGetError());
            d.closed = true;
        }
    }
}

void EglWindow::close() { d_->closed = true; }
bool EglWindow::closed() const { return d_->closed; }
int EglWindow::width() const { return d_->logical_w; }
int EglWindow::height() const { return d_->logical_h; }
int EglWindow::scale() const { return d_->scale; }

double EglWindow::seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - d_->t0).count();
}

// ---------------------------------------------------------------- EglOffscreen

struct EglOffscreen::Impl {
    EGLDisplay edpy = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    unsigned fbo = 0, rbo = 0;
    int w = 0, h = 0;
    std::string info;
};

EglOffscreen::EglOffscreen() : d_(std::make_unique<Impl>()) {}

EglOffscreen::~EglOffscreen() {
    Impl& d = *d_;
    if (d.edpy != EGL_NO_DISPLAY && d.ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(d.edpy, d.surf, d.surf, d.ctx);
        if (d.fbo) glDeleteFramebuffers(1, &d.fbo);
        if (d.rbo) glDeleteRenderbuffers(1, &d.rbo);
        eglMakeCurrent(d.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (d.surf != EGL_NO_SURFACE) eglDestroySurface(d.edpy, d.surf);
        eglDestroyContext(d.edpy, d.ctx);
        eglTerminate(d.edpy);
    }
}

bool EglOffscreen::init(int w, int h, std::string& err) {
    Impl& d = *d_;
    d.w = w;
    d.h = h;
    auto getPlatformDisplay =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    bool egl_ok = false;
    if (getPlatformDisplay) {
        d.edpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (d.edpy != EGL_NO_DISPLAY) egl_ok = eglInitialize(d.edpy, nullptr, nullptr);
    }
    if (!egl_ok) {
        d.edpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        egl_ok = d.edpy != EGL_NO_DISPLAY && eglInitialize(d.edpy, nullptr, nullptr);
    }
    if (!egl_ok) {
        err = "windowless EGL not available";
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = {EGL_SURFACE_TYPE,   EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_ES3_BIT, EGL_RED_SIZE,     8,
                         EGL_GREEN_SIZE,     8,               EGL_BLUE_SIZE,
                         8,                  EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(d.edpy, cfg_attr, &d.config, 1, &n) || n == 0) {
        cfg_attr[3] = EGL_OPENGL_ES2_BIT;
        eglChooseConfig(d.edpy, cfg_attr, &d.config, 1, &n);
    }
    if (n == 0) {
        err = "no offscreen EGL configuration";
        return false;
    }
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    d.ctx = eglCreateContext(d.edpy, d.config, EGL_NO_CONTEXT, ctx_attr);
    if (d.ctx == EGL_NO_CONTEXT) {
        err = "eglCreateContext(ES3) offscreen failed";
        return false;
    }
    if (!eglMakeCurrent(d.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, d.ctx)) {
        const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        d.surf = eglCreatePbufferSurface(d.edpy, d.config, pb);
        if (d.surf == EGL_NO_SURFACE || !eglMakeCurrent(d.edpy, d.surf, d.surf, d.ctx)) {
            err = "eglMakeCurrent offscreen failed";
            return false;
        }
    }

    glGenFramebuffers(1, &d.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, d.fbo);
    glGenRenderbuffers(1, &d.rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, d.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, d.rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        err = "incomplete offscreen framebuffer";
        return false;
    }
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    d.info = std::string(renderer ? renderer : "?") + " / " + (version ? version : "?");
    return true;
}

void EglOffscreen::bind() {
    Impl& d = *d_;
    eglMakeCurrent(d.edpy, d.surf, d.surf, d.ctx);
    glBindFramebuffer(GL_FRAMEBUFFER, d.fbo);
}

bool EglOffscreen::readRgb(std::vector<std::uint8_t>& rgb, std::string& err) {
    Impl& d = *d_;
    std::vector<std::uint8_t> rgba(std::size_t(d.w) * d.h * 4);
    glFinish();
    glReadPixels(0, 0, d.w, d.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "glReadPixels 0x%x", e);
        err = buf;
        return false;
    }
    rgb.resize(std::size_t(d.w) * d.h * 3);
    for (int y = 0; y < d.h; ++y) {
        const std::uint8_t* src = rgba.data() + std::size_t(d.h - 1 - y) * d.w * 4;
        std::uint8_t* dst = rgb.data() + std::size_t(y) * d.w * 3;
        for (int x = 0; x < d.w; ++x) {
            dst[3 * x + 0] = src[4 * x + 0];
            dst[3 * x + 1] = src[4 * x + 1];
            dst[3 * x + 2] = src[4 * x + 2];
        }
    }
    return true;
}

int EglOffscreen::width() const { return d_->w; }
int EglOffscreen::height() const { return d_->h; }
const char* EglOffscreen::glInfo() const { return d_->info.c_str(); }

}  // namespace fmiq
