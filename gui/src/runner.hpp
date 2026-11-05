// Running `sdr-load` as a subprocess, without a shell and without privileges.
//
// The window never elevates privileges: it runs the program as the user who opened
// it. If the operation needs root, the tool says so and the window shows the exact
// command to run.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glib.h>

namespace fmiq {

class Runner {
   public:
    // line, is_error (stderr)
    using LineFn = std::function<void(const std::string&, bool)>;
    using DoneFn = std::function<void(int code, bool signaled)>;

    Runner() = default;
    ~Runner();

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    // Returns false and fills err if it could not be started.
    bool start(const std::vector<std::string>& argv, LineFn on_line, DoneFn on_done,
               std::string* err);

    bool running() const { return pid_ >= 0 || !done_dispatched_; }
    void reset();

   private:
    struct Channel {
        Runner* self = nullptr;
        bool is_err = false;
        GIOChannel* ch = nullptr;
        guint watch = 0;
        std::string buf;
        bool closed = false;
    };

    static gboolean on_io(GIOChannel* source, GIOCondition cond, gpointer data);
    static void on_child(GPid pid, gint status, gpointer data);
    static gboolean dispatch_done(gpointer data);

    void drain(Channel* c, bool flush);
    void channel_closed(bool is_err);
    void maybe_finish();

    GPid pid_ = -1;
    Channel out_;
    Channel err_;
    bool out_closed_ = false;
    bool err_closed_ = false;
    bool exited_ = false;
    bool done_dispatched_ = true;
    int code_ = 0;
    bool signaled_ = false;
    LineFn line_;
    DoneFn done_;
    guint done_source_ = 0;
    guint child_source_ = 0;
};

}  // namespace fmiq
