#include "runner.hpp"

#include <cerrno>
#include <cstring>

#include <sys/types.h>
#include <sys/wait.h>

namespace fmiq {

Runner::~Runner() {
    // The sources may still be alive if the window is closed mid-command: they are
    // disconnected before releasing the channels, so no pointer is left dangling.
    if (done_source_) {
        g_source_remove(done_source_);
        done_source_ = 0;
    }
    if (child_source_) {
        g_source_remove(child_source_);
        child_source_ = 0;
    }
    if (out_.watch) {
        g_source_remove(out_.watch);
        out_.watch = 0;
    }
    if (err_.watch) {
        g_source_remove(err_.watch);
        err_.watch = 0;
    }
    if (out_.ch) {
        g_io_channel_unref(out_.ch);
        out_.ch = nullptr;
    }
    if (err_.ch) {
        g_io_channel_unref(err_.ch);
        err_.ch = nullptr;
    }
}

void Runner::reset() {
    if (pid_ >= 0 || !done_dispatched_) return;  // a command in flight is left alone
    if (done_source_) {
        g_source_remove(done_source_);
        done_source_ = 0;
    }
    line_ = nullptr;
    done_ = nullptr;
    out_.buf.clear();
    err_.buf.clear();
    out_closed_ = err_closed_ = false;
    exited_ = false;
    code_ = 0;
    signaled_ = false;
    done_dispatched_ = true;
}

bool Runner::start(const std::vector<std::string>& argv, LineFn on_line, DoneFn on_done,
                   std::string* err) {
    if (!done_dispatched_ || pid_ >= 0) {
        if (err) *err = "a command is already running";
        return false;
    }
    if (argv.empty()) {
        if (err) *err = "empty command";
        return false;
    }

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    GSpawnFlags flags = static_cast<GSpawnFlags>(G_SPAWN_DO_NOT_REAP_CHILD |
                                                 G_SPAWN_CLOEXEC_PIPES);
    if (argv[0].find('/') == std::string::npos) {
        flags = static_cast<GSpawnFlags>(flags | G_SPAWN_SEARCH_PATH);
    }

    int out_fd = -1;
    int err_fd = -1;
    GError* gerr = nullptr;
    if (!g_spawn_async_with_pipes(nullptr, args.data(), nullptr, flags, nullptr, nullptr, &pid_,
                                  nullptr, &out_fd, &err_fd, &gerr)) {
        if (err) *err = gerr ? gerr->message : "could not start the process";
        if (gerr) g_error_free(gerr);
        pid_ = -1;
        return false;
    }

    line_ = std::move(on_line);
    done_ = std::move(on_done);
    done_dispatched_ = false;
    exited_ = false;
    out_closed_ = err_closed_ = false;
    code_ = 0;
    signaled_ = false;
    out_.self = this;
    out_.is_err = false;
    out_.buf.clear();
    err_.self = this;
    err_.is_err = true;
    err_.buf.clear();

    out_.ch = g_io_channel_unix_new(out_fd);
    err_.ch = g_io_channel_unix_new(err_fd);
    for (Channel* c : {&out_, &err_}) {
        g_io_channel_set_encoding(c->ch, nullptr, nullptr);
        g_io_channel_set_buffered(c->ch, FALSE);
        g_io_channel_set_close_on_unref(c->ch, TRUE);
        c->closed = false;
        c->watch = g_io_add_watch(c->ch, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                  on_io, c);
    }
    child_source_ = g_child_watch_add(pid_, on_child, this);
    return true;
}

void Runner::drain(Channel* c, bool flush) {
    size_t pos = 0;
    while ((pos = c->buf.find('\n')) != std::string::npos) {
        std::string line = c->buf.substr(0, pos);
        c->buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line_) line_(line, c->is_err);
    }
    if (flush && !c->buf.empty()) {
        std::string line = c->buf;
        c->buf.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line_) line_(line, c->is_err);
    }
}

gboolean Runner::on_io(GIOChannel* source, GIOCondition cond, gpointer data) {
    Channel* c = static_cast<Channel*>(data);
    if (!c || !c->ch) return G_SOURCE_REMOVE;

    bool eof = false;
    for (;;) {
        gchar buf[4096];
        gsize n = 0;
        GError* e = nullptr;
        GIOStatus st = g_io_channel_read_chars(source, buf, sizeof(buf), &n, &e);
        if (n > 0) c->buf.append(buf, n);
        if (e) g_error_free(e);
        if (st == G_IO_STATUS_NORMAL) continue;
        if (st == G_IO_STATUS_EOF) eof = true;
        break;
    }

    const bool hangup = (cond & G_IO_HUP) != 0;
    c->self->drain(c, eof || hangup);
    if (eof || hangup) {
        c->closed = true;
        c->watch = 0;
        if (c->ch) {
            g_io_channel_unref(c->ch);
            c->ch = nullptr;
        }
        c->self->channel_closed(c->is_err);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void Runner::channel_closed(bool is_err) {
    if (is_err) err_closed_ = true;
    else out_closed_ = true;
    maybe_finish();
}

void Runner::on_child(GPid pid, gint status, gpointer data) {
    Runner* self = static_cast<Runner*>(data);
    self->child_source_ = 0;
    g_spawn_close_pid(pid);
    self->pid_ = -1;
    if (WIFEXITED(status)) {
        self->code_ = WEXITSTATUS(status);
        self->signaled_ = false;
    } else if (WIFSIGNALED(status)) {
        self->code_ = 128 + WTERMSIG(status);
        self->signaled_ = true;
    } else {
        self->code_ = -1;
    }
    self->exited_ = true;
    self->maybe_finish();
}

void Runner::maybe_finish() {
    if (!exited_ || !out_closed_ || !err_closed_ || done_dispatched_) return;
    done_source_ = g_idle_add(dispatch_done, this);
}

gboolean Runner::dispatch_done(gpointer data) {
    Runner* self = static_cast<Runner*>(data);
    self->done_source_ = 0;
    self->done_dispatched_ = true;
    DoneFn fn = self->done_;
    self->done_ = nullptr;
    self->line_ = nullptr;
    if (fn) fn(self->code_, self->signaled_);
    return G_SOURCE_REMOVE;
}

}  // namespace fmiq
