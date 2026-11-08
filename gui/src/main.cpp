// SDR mode load window.
//
// Everything the phone does is done by `sdr-load`: this window only builds the
// command, runs it as a subprocess and shows its output and its exit code. It does
// not duplicate the activation sequence and does not talk to the device.
//
// Privileges: when an action needs root and the window is not root, the command is
// handed to `pkexec` and the system's polkit agent draws the dialogue. The window
// never sees a password. If pkexec is missing or the user cancels, the exact command
// is shown with a button to copy it, so the window is never a dead end.
//
// Appearance: the stylesheet in gui/style.css is embedded in the binary and loaded
// above the session theme, so the window is dark whatever the desktop is.
#include <gtk/gtk.h>

#include <limits.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "actions.hpp"
#include "privileges.hpp"
#include "runner.hpp"
#include "status.hpp"
#include "theme.hpp"

namespace {

constexpr const char* kAppVersion = "furimodem-sdr 1.1";

struct Gui {
    GtkWidget* window = nullptr;
    GtkWidget* stack = nullptr;

    GtkWidget* mode_value = nullptr;
    GtkWidget* state_value = nullptr;
    GtkWidget* partition_value = nullptr;
    GtkWidget* image_value = nullptr;
    GtkWidget* image_check_value = nullptr;
    GtkWidget* profile_value = nullptr;
    GtkWidget* activation_value = nullptr;
    GtkWidget* ring_value = nullptr;
    GtkWidget* radios_value = nullptr;
    GtkWidget* updated_value = nullptr;
    GtkWidget* error_value = nullptr;
    GtkWidget* hint_value = nullptr;

    GtkWidget* live_row = nullptr;
    GtkWidget* live_label = nullptr;
    GtkWidget* live_button = nullptr;

    GtkWidget* progress = nullptr;
    GtkWidget* progress_label = nullptr;

    GtkWidget* banner = nullptr;
    GtkWidget* banner_headline = nullptr;
    GtkWidget* banner_detail = nullptr;
    GtkWidget* cmd_row = nullptr;
    GtkWidget* cmd_label = nullptr;

    GtkWidget* log_view = nullptr;
    GtkTextBuffer* log_buf = nullptr;
    GtkTextTag* tag_err = nullptr;
    GtkTextTag* tag_step = nullptr;

    GtkWidget* tool_label = nullptr;

    GtkWidget* btn_refresh = nullptr;
    GtkWidget* btn_flash = nullptr;
    GtkWidget* btn_enter = nullptr;
    GtkWidget* btn_activate = nullptr;
    GtkWidget* btn_deactivate = nullptr;
    GtkWidget* btn_restore = nullptr;
    GtkWidget* btn_app = nullptr;

    // The guide is what makes the window usable without reading anything else: one
    // sentence with the next step and the button that does it.
    GtkWidget* guide = nullptr;
    GtkWidget* guide_title = nullptr;
    GtkWidget* guide_detail = nullptr;

    fmiq::StatusInfo last_status;  // last parsed --status: the guide and the buttons live on it
    bool have_status = false;

    fmiq::Runner runner;
    fmiq::Action current = fmiq::Action::Status;
    fmiq::Plan plan;  // how the command in flight is being run
    bool running = false;
    bool quiet_status = false;
    // The state file and the partition describe different images: starting reception is
    // not offered until the mode image is written again.
    bool image_incoherent = false;

    bool tool_ok = false;
    std::string tool;
    std::string tool_origin;
    std::string version;

    std::string pkexec;  // absolute path to pkexec, empty when it is not installed
    std::string status_out;
    double fraction = 0.0;
};

enum class BannerKind { Ok, Error, Info };

void set_label(GtkWidget* label, const std::string& text) {
    gtk_label_set_text(GTK_LABEL(label), text.c_str());
}

void set_named_class(GtkWidget* w, const char* name, bool on) {
    GtkStyleContext* ctx = gtk_widget_get_style_context(w);
    if (on) {
        gtk_style_context_add_class(ctx, name);
    } else {
        gtk_style_context_remove_class(ctx, name);
    }
}

// Shows or hides a block that starts hidden at start-up.
// gtk_widget_show_all() does nothing on a widget marked no-show-all, so the mark has
// to be cleared first; that is also why the children come along without an explicit
// loop over them.
void reveal(GtkWidget* w, bool visible) {
    gtk_widget_set_no_show_all(w, !visible);
    if (visible) {
        gtk_widget_show_all(w);
    } else {
        gtk_widget_hide(w);
    }
}

std::string shorten(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

std::string join_nonempty(const std::string& a, const std::string& b, const char* sep) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + sep + b;
}

std::string state_gloss(const std::string& raw) {
    if (raw == "ok") return "image loaded";
    if (raw == "sdr-active") return "SDR mode loaded";
    if (raw == "entering") return "entering SDR mode";
    if (raw == "flashing") return "writing the firmware";
    if (raw == "restoring") return "going back to the normal phone";
    if (raw == "recovering") return "recovering the modem";
    if (raw == "activate-failed") return "activation failed";
    if (raw == "flash-failed") return "the flash failed";
    return "";
}

// --- tool discovery --------------------------------------------------------

bool resolve_tool(Gui* g) {
    g->tool.clear();
    g->tool_origin.clear();
    g->tool_ok = false;

    const char* env = g_getenv("SDR_LOAD");
    if (env && *env) {
        g->tool = env;
        if (g_file_test(env, G_FILE_TEST_IS_EXECUTABLE)) {
            g->tool_origin = "SDR_LOAD";
            g->tool_ok = true;
            return true;
        }
        g->tool_origin = "SDR_LOAD (not executable)";
        return false;
    }

    char exe[PATH_MAX] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        std::string dir(exe, static_cast<size_t>(n));
        const size_t slash = dir.rfind('/');
        if (slash != std::string::npos) dir.resize(slash);
        const std::string candidate = dir + "/sdr-load";
        if (g_file_test(candidate.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
            g->tool = candidate;
            g->tool_origin = "next to the window";
            g->tool_ok = true;
            return true;
        }
    }

    gchar* found = g_find_program_in_path("sdr-load");
    if (found) {
        g->tool = found;
        g_free(found);
        g->tool_origin = "PATH";
        g->tool_ok = true;
        return true;
    }

    g->tool = "sdr-load";
    g->tool_origin = "not found";
    return false;
}

// pkexec is looked for once at start-up. Nothing is asked of the user here: only the
// path is remembered, and the dialogue (if any) is opened when an operation needs it.
void resolve_privileges(Gui* g) {
    g->pkexec.clear();
    if (::geteuid() == 0) return;  // already root: nobody to ask
    g->pkexec = fmiq::find_in_path("pkexec");
}

std::string status_version(const std::string& raw) {
    const size_t eol = raw.find('\n');
    std::string first = raw.substr(0, eol == std::string::npos ? raw.size() : eol);
    if (first.rfind("sdr-load ", 0) != 0) return "";
    const size_t p = first.find(" --");
    if (p != std::string::npos) first.resize(p);
    return first;
}

std::string privilege_caption(Gui* g) {
    if (::geteuid() == 0) return "root session, commands run directly";
    if (!g->pkexec.empty()) {
        return "polkit (" + g->pkexec + "): the system asks for authorisation";
    }
    return "none: pkexec is not installed, the commands are shown, not run";
}

void update_tool_caption(Gui* g) {
    std::string text = "Tool: " + g->tool;
    if (!g->tool_ok) {
        text += "  (not found)";
    } else {
        if (!g->version.empty()) text += "  ·  " + g->version;
        if (::geteuid() == 0) text += "  ·  root session";
    }
    text += "\nPrivileges: " + privilege_caption(g);
    set_label(g->tool_label, text);
}

// --- status panel ----------------------------------------------------------

void set_value(GtkWidget* label, const std::string& text, const std::string& tooltip) {
    set_label(label, text.empty() ? "—" : text);
    gtk_widget_set_tooltip_text(label, tooltip.empty() ? nullptr : tooltip.c_str());
}

// Defined with the buttons, below: the status panel needs it to disable "start
// reception" when the partition does not hold a mode image.
void set_buttons_enabled(Gui* g);
void update_guide(Gui* g);

void apply_status(Gui* g, const fmiq::StatusInfo& s) {
    g->last_status = s;
    g->have_status = s.parsed;
    if (!s.parsed) {
        g->image_incoherent = false;
        set_value(g->mode_value, "Output not recognised", "");
        set_value(g->state_value, "—", "");
        set_value(g->partition_value, "—", "");
        set_value(g->image_value, "—", "");
        set_value(g->image_check_value, "—", "");
        set_value(g->profile_value, "—", "");
        set_value(g->activation_value, "—", "");
        set_value(g->ring_value, "—", "");
        set_value(g->radios_value, "—", "");
        set_value(g->updated_value, "—", "");
        set_value(g->error_value, "—", "");
        set_value(g->hint_value, "—", "");
        reveal(g->live_row, false);
        gtk_button_set_label(GTK_BUTTON(g->btn_flash), "1. Load SDR image...");
        set_named_class(g->btn_flash, "suggested-action", false);
        set_buttons_enabled(g);
        update_guide(g);
        return;
    }

    set_value(g->mode_value, fmiq::mode_text(s), s.observed);

    std::string state = s.state;
    if (state.empty() || state.rfind("none", 0) == 0) {
        state = "no saved state";
    } else {
        state = join_nonempty(state, state_gloss(state), "  ·  ");
    }
    set_value(g->state_value, state, s.pending.empty() ? "" : "pending: " + s.pending);

    // What the partition holds (the tool hashed it), never what the state remembers.
    set_value(g->partition_value, s.partition.empty() ? "—" : s.partition, s.partition);
    std::string img;
    if (!s.image_now.empty() && s.image_now.rfind("not readable", 0) != 0) {
        img = shorten(s.image_now, 16);
        if (!s.image_kind.empty()) img = join_nonempty(img, s.image_kind, "  ·  ");
    } else if (!s.image_now.empty()) {
        img = s.image_now;
    }
    set_value(g->image_value, img, s.image_now.empty() ? ""
                                                       : s.image_now + "  " + s.image_size);
    set_value(g->image_check_value, s.image_check, s.image_check);

    g->image_incoherent = s.image_incoherent;
    if (s.image_incoherent) {
        // The correct action is to write the image again (with its reboot warning), not
        // to activate: activation cannot work on an image that is not the mode one.
        gtk_button_set_label(GTK_BUTTON(g->btn_flash), "1. Write the image again...");
        gtk_widget_set_tooltip_text(
            g->btn_flash,
            "Choose the mode image and write it into the partition; a reboot is needed afterwards");
        set_named_class(g->btn_flash, "suggested-action", true);
        const std::string kind = s.image_kind.empty() ? "another image" : s.image_kind;
        gtk_widget_set_tooltip_text(
            g->btn_activate,
            ("The partition holds " + kind + ": write the mode image before starting "
                                          "reception").c_str());
    } else {
        gtk_button_set_label(GTK_BUTTON(g->btn_flash), "1. Load SDR image...");
        gtk_widget_set_tooltip_text(g->btn_flash,
                                    "Writes the firmware partition with the chosen image");
        set_named_class(g->btn_flash, "suggested-action", false);
        gtk_widget_set_tooltip_text(g->btn_activate, "Runs the nine activation steps");
    }
    set_buttons_enabled(g);
    update_guide(g);

    std::string profile = s.profile;
    if (!s.profile_known && !profile.empty()) profile = "UNKNOWN";
    if (profile.empty()) profile = "—";
    std::string extra = s.profile_extra;
    const std::string extra_full = extra;
    if (extra.size() > 90) extra = extra.substr(0, 87) + "...";
    const std::string profile_tail = extra.empty() ? std::string() : "(" + extra + ")";
    set_value(g->profile_value, join_nonempty(profile, profile_tail, "  ·  "), extra_full);

    std::string act;
    if (!s.loops.empty()) act = "loops: " + s.loops;
    if (!s.rails.empty()) act = join_nonempty(act, "rails: " + s.rails, "  ·  ");
    set_value(g->activation_value, act, "");

    std::string ring;
    if (s.ring == "late") ring = "late (with dump)";
    else if (s.ring == "quiet") ring = "quiet (no dump)";
    else if (!s.ring.empty()) ring = s.ring;
    set_value(g->ring_value, ring, "");

    set_value(g->radios_value, s.radios, "");
    set_value(g->updated_value, s.updated, s.updated);
    set_value(g->error_value, s.last_error == "-" ? "" : s.last_error, "");
    set_value(g->hint_value, s.hint, "");

    // A privileged read is also what is missing when the partition could not be hashed:
    // the guide says "Read with privileges" and the button has to be there.
    const bool image_unreadable = s.image_now.rfind("not readable", 0) == 0;
    const bool show_live = (!s.observable || image_unreadable) && ::geteuid() != 0;
    if (!show_live) {
        reveal(g->live_row, false);
        return;
    }
    const std::vector<std::string> argv = fmiq::argv_for(fmiq::Action::Status, g->tool);
    const fmiq::Plan plan = fmiq::plan_run(true, argv, false, g->pkexec);
    set_label(g->live_label, fmiq::copyable_command(plan, argv));
    // The row starts hidden: reveal() clears the mark and shows it with its children,
    // and the button is hidden afterwards when there is no pkexec to ask.
    reveal(g->live_row, true);
    gtk_widget_set_visible(g->live_button, plan.route == fmiq::Route::Pkexec);
}

// --- log and progress ------------------------------------------------------

void log_append(Gui* g, const std::string& line, bool is_err) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(g->log_buf, &end);
    if (is_err) {
        gtk_text_buffer_insert_with_tags(g->log_buf, &end, line.c_str(), -1, g->tag_err, nullptr);
    } else if (!line.empty() && line.front() == '[') {
        gtk_text_buffer_insert_with_tags(g->log_buf, &end, line.c_str(), -1, g->tag_step, nullptr);
    } else {
        gtk_text_buffer_insert(g->log_buf, &end, line.c_str(), -1);
    }
    gtk_text_buffer_insert(g->log_buf, &end, "\n", -1);

    GtkTextIter last;
    gtk_text_buffer_get_end_iter(g->log_buf, &last);
    if (gtk_text_buffer_get_line_count(g->log_buf) > 4000) {
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(g->log_buf, &start);
        GtkTextIter cut = start;
        gtk_text_iter_forward_lines(&cut, 500);
        gtk_text_buffer_delete(g->log_buf, &start, &cut);
    }

    GtkTextMark* mark = gtk_text_buffer_create_mark(g->log_buf, nullptr, &last, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(g->log_view), mark);
    gtk_text_buffer_delete_mark(g->log_buf, mark);
}

void note_step(Gui* g, const std::string& line) {
    int i = 0, n = 0;
    if (line.size() < 5 || line[0] != '[') return;
    if (std::sscanf(line.c_str(), "[%d/%d]", &i, &n) != 2 || n <= 0 || i < 0) return;
    double f = static_cast<double>(i) / static_cast<double>(n);
    if (f > 1.0) f = 1.0;
    g->fraction = f;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g->progress), f);

    const size_t close = line.find(']');
    std::string what = close == std::string::npos ? "" : line.substr(close + 1);
    while (!what.empty() && what.front() == ' ') what.erase(what.begin());
    set_label(g->progress_label,
              "Step " + std::to_string(i) + " of " + std::to_string(n) + ": " + what);
}

// --- result banner ---------------------------------------------------------

void show_banner(Gui* g, BannerKind kind, const std::string& headline,
                 const std::string& detail, const std::string& command) {
    set_label(g->banner_headline, headline);
    set_label(g->banner_detail, detail);

    set_named_class(g->banner, "banner-ok", kind == BannerKind::Ok);
    set_named_class(g->banner, "banner-error", kind == BannerKind::Error);
    set_named_class(g->banner, "banner-info", kind == BannerKind::Info);

    if (!command.empty()) {
        set_label(g->cmd_label, command);
    }
    // The banner and its command row both start hidden and are marked no-show-all, so
    // each one is revealed on its own: show_all() skips a widget carrying that mark.
    reveal(g->banner, true);
    reveal(g->cmd_row, !command.empty());
}

void hide_banner(Gui* g) { reveal(g->banner, false); }

// The partition holds a mode image the tool knows: only then does entering the mode
// or starting reception make sense. On the stock image both can only fail.
bool have_mode_image(const Gui* g) {
    return g->have_status && g->last_status.profile_known && !g->last_status.image_incoherent;
}

void set_buttons_enabled(Gui* g) {
    const bool idle = !g->running;
    const bool act = idle && g->tool_ok;
    const bool image = have_mode_image(g);
    gtk_widget_set_sensitive(g->btn_refresh, idle);
    gtk_widget_set_sensitive(g->btn_flash, act);
    // A sweep image must keep the modem in service: entering SDR mode would stop it.
    gtk_widget_set_sensitive(g->btn_enter, act && image && !g->last_status.scan);
    // With a partition that does not hold a mode image, "start reception" can only fail:
    // the action offered is to write the image, not to activate.
    gtk_widget_set_sensitive(g->btn_activate, act && image);
    gtk_widget_set_sensitive(g->btn_deactivate, act);
    gtk_widget_set_sensitive(g->btn_restore, act);
    // The app is only useful once the modem is in SDR mode; it is not privileged.
    gtk_widget_set_sensitive(g->btn_app, idle && g->tool_ok && g->have_status &&
                                              g->last_status.sdr_active);
}

// One and only one suggested button, the one that moves the phone to the next step.
// Entering the mode is not a step of its own: --activate does it first when the modem
// is not there yet, and with a sweep image it must not be done at all.
enum class Step { Unknown, LoadImage, StartReception, Running };

Step current_step(const Gui* g) {
    if (!g->have_status) return Step::Unknown;
    const fmiq::StatusInfo& s = g->last_status;
    if (s.image_incoherent) return Step::LoadImage;
    // Unreadable partition (no privileges) or unverified image: nothing can be decided
    // about the next step yet, and offering "load the image" could overwrite a good one.
    const bool unreadable = s.image_now.empty() || s.image_now.rfind("not readable", 0) == 0;
    if (unreadable || s.image_unverified) return Step::Unknown;
    if (!s.profile_known) return Step::LoadImage;
    // A sweep image never arms the loops: for it, "running" is the ring publishing.
    if (s.sdr_active && (s.loops == "armed" || s.ring == "late")) return Step::Running;
    return Step::StartReception;
}

void update_guide(Gui* g) {
    if (!g->guide) return;
    set_named_class(g->btn_flash, "suggested-action", false);
    set_named_class(g->btn_enter, "suggested-action", false);
    set_named_class(g->btn_activate, "suggested-action", false);
    set_named_class(g->btn_app, "suggested-action", false);

    std::string title;
    std::string detail;
    switch (current_step(g)) {
        case Step::Unknown:
            title = "Read the modem state";
            detail = "The window needs to know which image the firmware partition holds "
                     "before it can name the next step. If the state could not be read, "
                     "use \"Read with privileges\" in the State tab; otherwise press the "
                     "refresh button.";
            break;
        case Step::LoadImage:
            title = "Step 1 of 2: load the SDR mode image";
            detail = g->last_status.image_incoherent
                         ? "The saved state and the partition disagree. Write the mode image "
                           "again with \"1. Write the image again...\": a reboot is needed afterwards."
                         : "The partition does not hold an SDR mode image, so entering the "
                           "mode would leave the modem with nothing to capture. Choose the "
                           "mode image with \"1. Load SDR image...\": it is written once and "
                           "a reboot is needed afterwards.";
            set_named_class(g->btn_flash, "suggested-action", true);
            break;
        case Step::StartReception:
            title = "Step 2 of 2: start reception";
            detail = g->last_status.scan
                         ? "This is a sweep image: the modem stays in service and the phone's "
                           "own frequency scan produces the captures. The activation starts "
                           "the scan and checks the ring. When it finishes, open the SDR app."
                         : "The mode image is in place. This enters SDR mode if it is not "
                           "entered yet (the modem stops working as a phone: no calls, no "
                           "mobile data) and runs the activation steps, with a check after "
                           "each one. Nothing is written. When it finishes, open the SDR app.";
            set_named_class(g->btn_activate, "suggested-action", true);
            break;
        case Step::Running:
            title = "Reception is running";
            detail = "Open the SDR app to see the waterfall. To go back to a normal phone, "
                     "stop reception and then restore to stock.";
            set_named_class(g->btn_app, "suggested-action", true);
            break;
    }
    set_label(g->guide_title, title);
    set_label(g->guide_detail, detail);
}

// --- execution -------------------------------------------------------------

void on_tool_line(Gui* g, const std::string& line, bool is_err) {
    if (g->current == fmiq::Action::Status && !is_err) {
        g->status_out += line;
        g->status_out += "\n";
    }
    if (g->quiet_status) return;
    log_append(g, line, is_err);
    if (!is_err) note_step(g, line);
}

void apply_status_and_caption(Gui* g) {
    fmiq::StatusInfo s = fmiq::parse_status(g->status_out);
    const std::string v = status_version(g->status_out);
    if (!v.empty()) g->version = v;
    update_tool_caption(g);
    apply_status(g, s);
}

void start_status(Gui* g, bool quiet);
void show_privilege(Gui* g, const fmiq::Plan& plan, const std::vector<std::string>& tool_argv);

gboolean idle_refresh(gpointer data) {
    start_status(static_cast<Gui*>(data), true);
    return G_SOURCE_REMOVE;
}

// pkexec's own failures (dialogue dismissed, no agent, policy missing) are not the
// tool's exit code: they get their own sentence and, above all, the command to copy.
// The window goes back to idle in every case, so it is never left waiting.
bool show_auth_failure(Gui* g, int code) {
    // Only when pkexec was the one running it: 126/127 are its codes, not the tool's.
    if (g->plan.route != fmiq::Route::Pkexec) return false;
    const fmiq::AuthFailure failure = fmiq::classify_pkexec_exit(code);
    if (failure == fmiq::AuthFailure::None) return false;

    const fmiq::AuthText text = fmiq::auth_failure_text(failure);
    const std::string cmd = fmiq::copyable_command(g->plan, g->plan.argv);
    show_banner(g, BannerKind::Error, text.headline, text.detail, cmd);
    log_append(g, "— " + text.headline, true);
    log_append(g, cmd, false);
    set_label(g->progress_label, "Not run: no privileges");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g->progress), 0.0);
    return true;
}

void on_tool_done(Gui* g, fmiq::Action action, bool quiet, int code, bool signaled) {
    g->running = false;
    g->quiet_status = false;
    set_buttons_enabled(g);

    if (show_auth_failure(g, code)) return;

    fmiq::ExitInfo info = fmiq::describe_exit(action, code);
    if (signaled) info.detail += " The process received a signal.";

    if (action == fmiq::Action::Status) {
        if (code == 0) {
            apply_status_and_caption(g);
            fmiq::StatusInfo s = fmiq::parse_status(g->status_out);
            if (!s.parsed && !quiet) {
                show_banner(g, BannerKind::Info, "Unrecognised output",
                            "The tool answered, but this window does not understand the "
                            "format. The log has the full output.",
                            "");
            } else if (s.image_incoherent && !quiet) {
                // The state says SDR mode is loaded and the partition holds another
                // image: the way out is to write the image, not to repeat anything.
                show_banner(g, BannerKind::Error, "The state and the partition disagree",
                            s.image_check +
                                ".  The state file is only a record of what the tool did; the "
                                "partition is what counts. Reception cannot start until the mode "
                                "image is written again, and writing it needs a reboot afterwards.",
                            "");
            }
        } else if (!quiet) {
            show_banner(g, BannerKind::Error, info.headline, info.detail, "");
        }
        if (!quiet) {
            g->fraction = code == 0 ? 1.0 : 0.0;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g->progress), g->fraction);
            set_label(g->progress_label, code == 0 ? "State updated" : info.headline);
        }
        return;
    }

    g->fraction = info.ok ? 1.0 : g->fraction;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g->progress), g->fraction);
    set_label(g->progress_label, info.ok ? "Ready" : "With issues");

    std::string command;
    if (info.reboot) {
        command = (::geteuid() == 0 ? "systemctl reboot" : "sudo systemctl reboot");
    }
    show_banner(g, info.ok ? BannerKind::Ok : BannerKind::Error, info.headline, info.detail,
                command);

    // After touching the phone, the panel is re-read without overwriting the result.
    g_idle_add(idle_refresh, g);
}

void begin_run(Gui* g, fmiq::Action action, const std::vector<std::string>& argv, bool quiet,
               const fmiq::Plan& plan) {
    g->current = action;
    g->plan = plan;
    g->quiet_status = quiet;
    g->running = true;
    g->fraction = 0.0;
    set_buttons_enabled(g);

    if (action != fmiq::Action::Status) {
        gtk_stack_set_visible_child_name(GTK_STACK(g->stack), "operations");
    }

    if (!quiet) {
        gtk_text_buffer_set_text(g->log_buf, "", -1);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g->progress), 0.0);
        if (plan.route == fmiq::Route::Pkexec) {
            set_label(g->progress_label,
                      "Waiting for the system authorisation: " + fmiq::command_text(argv));
        } else if (action == fmiq::Action::Status) {
            set_label(g->progress_label, "Reading the state...");
        } else {
            set_label(g->progress_label, "Running: " + fmiq::command_text(argv));
        }
        hide_banner(g);
    }

    std::string err;
    fmiq::Runner::LineFn on_line = [g](const std::string& line, bool is_err) {
        on_tool_line(g, line, is_err);
    };
    fmiq::Runner::DoneFn on_done = [g, action, quiet](int code, bool signaled) {
        on_tool_done(g, action, quiet, code, signaled);
    };

    if (!g->runner.start(argv, on_line, on_done, &err)) {
        g->running = false;
        g->quiet_status = false;
        set_buttons_enabled(g);
        const std::string cmd = fmiq::command_text(argv);
        show_banner(g, BannerKind::Error,
                    plan.route == fmiq::Route::Pkexec ? "pkexec could not be started"
                                                      : "Could not start sdr-load",
                    err + ". The command was: " + cmd, "");
        set_label(g->progress_label, "Not run");
    }
}

void start_status(Gui* g, bool quiet) {
    if (g->running) return;
    const bool found = resolve_tool(g);
    update_tool_caption(g);
    if (!found) {
        set_buttons_enabled(g);
        if (!quiet) {
            show_banner(g, BannerKind::Error, "sdr-load not found",
                        "Put the binary next to this window, in PATH, or set SDR_LOAD to "
                        "its path. Looked for: " + g->tool + " (" + g->tool_origin + ").",
                        "");
        }
        return;
    }
    g->status_out.clear();
    if (!quiet) {
        gtk_stack_set_visible_child_name(GTK_STACK(g->stack), "state");
    }
    const std::vector<std::string> argv = fmiq::argv_for(fmiq::Action::Status, g->tool);
    // Reading the state does not need privileges: the tool answers with the saved
    // state and says which part it cannot see. The live part is read on demand, with
    // the button on the State tab.
    const fmiq::Plan plan = fmiq::plan_run(false, argv, ::geteuid() == 0, g->pkexec);
    begin_run(g, fmiq::Action::Status, plan.argv, quiet, plan);
}

// The live modem state is behind root. Rather than sending the user to a terminal,
// the same read is repeated through the polkit dialogue. It is not done on start-up:
// a password dialogue in the face on every launch would be worse than the delay.
void start_privileged_status(Gui* g) {
    if (g->running) return;
    if (!resolve_tool(g)) {
        update_tool_caption(g);
        set_buttons_enabled(g);
        show_banner(g, BannerKind::Error, "sdr-load not found",
                    "Put the binary next to this window, in PATH, or set SDR_LOAD to its "
                    "path. Looked for: " + g->tool + " (" + g->tool_origin + ").",
                    "");
        return;
    }
    update_tool_caption(g);

    const std::vector<std::string> argv = fmiq::argv_for(fmiq::Action::Status, g->tool);
    const fmiq::Plan plan = fmiq::plan_run(true, argv, ::geteuid() == 0, g->pkexec);
    if (plan.route == fmiq::Route::NoPkexec) {
        show_privilege(g, plan, argv);
        return;
    }
    g->status_out.clear();
    begin_run(g, fmiq::Action::Status, plan.argv, false, plan);
}

void show_privilege(Gui* g, const fmiq::Plan& plan, const std::vector<std::string>& tool_argv) {
    const std::string cmd = fmiq::copyable_command(plan, tool_argv);
    show_banner(g, BannerKind::Error, "Not run: no way to ask for privileges",
                "This operation needs administrator rights and pkexec is not installed, so "
                "the window cannot open the system dialogue. Install pkexec (polkit) to run "
                "it from here, or copy the command and run it in a terminal.",
                cmd);
    log_append(g, "— not run: no privileges", true);
    log_append(g, cmd, false);
}

void start_action(Gui* g, fmiq::Action action, const std::string& image) {
    if (g->running) return;
    if (!resolve_tool(g)) {
        update_tool_caption(g);
        set_buttons_enabled(g);
        show_banner(g, BannerKind::Error, "sdr-load not found",
                    "Put the binary next to this window, in PATH, or set SDR_LOAD to its "
                    "path. Looked for: " + g->tool + " (" + g->tool_origin + ").",
                    "");
        return;
    }
    update_tool_caption(g);

    if (action == fmiq::Action::FlashImage && image.empty()) {
        show_banner(g, BannerKind::Error, "Image missing",
                    "Choose an image file before loading.", "");
        return;
    }

    const fmiq::ActionSpec& spec = fmiq::spec_for(action);
    const std::vector<std::string> argv = fmiq::argv_for(action, g->tool, image);
    // root: straight through. Not root: pkexec and the system dialogue. No pkexec:
    // nothing is run and the exact command is offered.
    const fmiq::Plan plan = fmiq::plan_run(spec.needs_root, argv, ::geteuid() == 0, g->pkexec);
    if (plan.route == fmiq::Route::NoPkexec) {
        show_privilege(g, plan, argv);
        return;
    }
    begin_run(g, action, plan.argv, false, plan);
}

// --- dialogs ---------------------------------------------------------------

bool confirm_action(Gui* g, const fmiq::ActionSpec& spec, const std::string& image) {
    const std::string secondary =
        image.empty() ? std::string(spec.detail) : std::string(spec.detail) + "\n\nImage: " + image;
    GtkWidget* dialog = gtk_message_dialog_new(
        GTK_WINDOW(g->window),
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", spec.question);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary.c_str());
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget* go = gtk_dialog_add_button(GTK_DIALOG(dialog), "Continue", GTK_RESPONSE_ACCEPT);
    if (spec.dangerous) {
        gtk_style_context_add_class(gtk_widget_get_style_context(go), "destructive-action");
    }
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_ACCEPT;
}

void on_copy_clicked(GtkButton*, gpointer data) {
    GtkWidget* label = GTK_WIDGET(data);
    const char* text = gtk_label_get_text(GTK_LABEL(label));
    if (!text || !*text) return;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), text, -1);
}

// --- window actions --------------------------------------------------------

void on_refresh_clicked(GtkButton*, gpointer data) {
    Gui* g = static_cast<Gui*>(data);
    start_status(g, false);
}

void on_live_clicked(GtkButton*, gpointer data) {
    start_privileged_status(static_cast<Gui*>(data));
}

void on_flash_clicked(GtkButton*, gpointer data) {
    Gui* g = static_cast<Gui*>(data);
    if (g->running) return;

    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Choose SDR mode image", GTK_WINDOW(g->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL, "Choose", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Firmware images (*.img, *.bin)");
    gtk_file_filter_add_pattern(filter, "*.img");
    gtk_file_filter_add_pattern(filter, "*.bin");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    GtkFileFilter* all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all);

    std::string image;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar* chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (chosen) image = chosen;
        g_free(chosen);
    }
    gtk_widget_destroy(dialog);
    if (image.empty()) return;

    if (!g_file_test(image.c_str(), G_FILE_TEST_IS_REGULAR)) {
        show_banner(g, BannerKind::Error, "The image cannot be read",
                    "It does not exist or is not a regular file: " + image, "");
        return;
    }
    if (!confirm_action(g, fmiq::spec_for(fmiq::Action::FlashImage), image)) return;
    start_action(g, fmiq::Action::FlashImage, image);
}

void on_simple_action(GtkButton*, gpointer data) {
    // data points to a (Gui*, Action) pair kept alive by the window object.
    auto* pair = static_cast<std::pair<Gui*, fmiq::Action>*>(data);
    Gui* g = pair->first;
    if (g->running) return;
    const fmiq::Action action = pair->second;
    if (!confirm_action(g, fmiq::spec_for(action), "")) return;
    start_action(g, action, "");
}

gboolean on_delete_event(GtkWidget*, GdkEvent*, gpointer data) {
    Gui* g = static_cast<Gui*>(data);
    if (!g->running) return FALSE;
    GtkWidget* dialog = gtk_message_dialog_new(
        GTK_WINDOW(g->window),
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "An operation is in progress");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "The window will close when it finishes. Closing it now would cut the communication "
        "with the tool and could interrupt a write.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return TRUE;
}

// --- window construction ---------------------------------------------------

// Opens the waterfall app. It is a normal GUI in the same session: nothing here needs
// root, and if it is not installed the banner says so instead of failing silently.
void on_app_clicked(GtkWidget*, gpointer data) {
    Gui* g = static_cast<Gui*>(data);
    gchar* argv[] = {const_cast<gchar*>("fmiq-app"), nullptr};
    GError* err = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                       nullptr, &err)) {
        show_banner(g, BannerKind::Error, "The SDR app could not be started",
                    err ? err->message : "fmiq-app is not installed", "fmiq-app");
        if (err) g_error_free(err);
    }
}

// The window brings its own colours. Two things are done, in this order:
//   1. the GTK setting that asks for the dark variant of the theme, so the widgets
//      that still get their colours from the session theme pick the dark ones;
//   2. the embedded stylesheet, at USER priority, which fixes the background,
//      surfaces, text and borders whatever the desktop is using. This is what makes
//      the window dark on a light desktop instead of merely "not light".
void install_theme() {
    GtkSettings* settings = gtk_settings_get_default();
    if (settings) {
        // Same as gtk_settings_set_property(), which is not public in GTK3: the
        // setting is a GObject property of GtkSettings.
        g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, nullptr);
    }

    GtkCssProvider* provider = gtk_css_provider_new();
    GError* err = nullptr;
    const char* css = fmiq::dark_css();
    gtk_css_provider_load_from_data(provider, css ? css : "", -1, &err);
    if (err) {
        // A broken stylesheet must not take the window down: it would open with the
        // session colours, which is worse but usable.
        std::fprintf(stderr, "furimodem-sdr: stylesheet: %s\n", err->message);
        g_error_free(err);
    }
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(provider);
}

GtkWidget* add_status_row(GtkGrid* grid, int row, const char* title, GtkWidget** value) {
    GtkWidget* key = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(key), 0.0);
    gtk_widget_set_valign(key, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(key), "dim");
    gtk_grid_attach(grid, key, 0, row, 1, 1);

    GtkWidget* val = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(val), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(val), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(val), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(val), 30);
    gtk_widget_set_hexpand(val, TRUE);
    gtk_grid_attach(grid, val, 1, row, 1, 1);
    *value = val;
    return val;
}

GtkWidget* make_section(const char* title, GtkWidget* child) {
    GtkWidget* frame = gtk_frame_new(nullptr);
    char markup[128];
    std::snprintf(markup, sizeof(markup), "<b>%s</b>", title);
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_frame_set_label_widget(GTK_FRAME(frame), label);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

GtkWidget* make_command_row(GtkWidget** label_out) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "cmd");
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);

    GtkWidget* copy = gtk_button_new_with_label("Copy command");
    gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
    g_signal_connect(copy, "clicked", G_CALLBACK(on_copy_clicked), label);
    gtk_box_pack_end(GTK_BOX(row), copy, FALSE, FALSE, 0);

    *label_out = label;
    return row;
}

void build_window(Gui* g, GtkApplication* app) {
    g->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g->window), "SDR Mode");
    gtk_window_set_default_size(GTK_WINDOW(g->window), 400, 720);

    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "SDR Mode");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Load, activate and revert");
    g->btn_refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(g->btn_refresh, "Refresh state");
    gtk_widget_set_valign(g->btn_refresh, GTK_ALIGN_CENTER);
    g_signal_connect(g->btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), g);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), g->btn_refresh);
    gtk_window_set_titlebar(GTK_WINDOW(g->window), header);

    GtkWidget* outer = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(g->window), outer);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_container_add(GTK_CONTAINER(outer), content);

    // Two pages: the state is read without noise and the operations keep their
    // progress and their log always in sight.
    g->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(g->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_transition_duration(GTK_STACK(g->stack), 0);
    GtkWidget* switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(g->stack));
    gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(content), switcher, FALSE, FALSE, 0);

    // The guide is the first thing read: the next step in one sentence and the button
    // that does it, highlighted. Everything else is detail underneath.
    g->guide = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(g->guide), "guide");
    g->guide_title = gtk_label_new("Read the modem state");
    gtk_label_set_xalign(GTK_LABEL(g->guide_title), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g->guide_title), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g->guide_title), "guide-title");
    g->guide_detail = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g->guide_detail), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g->guide_detail), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(g->guide_detail), 44);
    gtk_style_context_add_class(gtk_widget_get_style_context(g->guide_detail), "dim");
    gtk_box_pack_start(GTK_BOX(g->guide), g->guide_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g->guide), g->guide_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), g->guide, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), g->stack, TRUE, TRUE, 0);

    GtkWidget* status_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* ops_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    // --- state ---
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    int row = 0;
    add_status_row(GTK_GRID(grid), row++, "Mode", &g->mode_value);
    // The image rows are what the partition holds (hashed by the tool), not what the
    // state file remembers: that is where the incoherence shows up.
    add_status_row(GTK_GRID(grid), row++, "Image now", &g->image_value);
    add_status_row(GTK_GRID(grid), row++, "Image check", &g->image_check_value);
    add_status_row(GTK_GRID(grid), row++, "Next", &g->hint_value);

    // Everything needed to diagnose stays here, folded away: the four rows above are
    // what a person needs to know which step comes next.
    GtkWidget* details_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(details_grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(details_grid), 4);
    int drow = 0;
    add_status_row(GTK_GRID(details_grid), drow++, "Saved state", &g->state_value);
    add_status_row(GTK_GRID(details_grid), drow++, "Partition", &g->partition_value);
    add_status_row(GTK_GRID(details_grid), drow++, "Profile", &g->profile_value);
    add_status_row(GTK_GRID(details_grid), drow++, "Activation", &g->activation_value);
    add_status_row(GTK_GRID(details_grid), drow++, "Ring", &g->ring_value);
    add_status_row(GTK_GRID(details_grid), drow++, "Radios", &g->radios_value);
    add_status_row(GTK_GRID(details_grid), drow++, "Updated", &g->updated_value);
    add_status_row(GTK_GRID(details_grid), drow++, "Last error", &g->error_value);

    GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(status_box), grid, FALSE, FALSE, 0);
    GtkWidget* details = gtk_expander_new("Technical details");
    GtkWidget* details_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(details), details_box);
    gtk_box_pack_start(GTK_BOX(details_box), details_grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_box), details, FALSE, FALSE, 0);

    g->live_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* live_intro = gtk_label_new(
        "The live modem state is only visible with administrator rights. The system can ask "
        "for them, or you can run this command in a terminal.");
    gtk_label_set_xalign(GTK_LABEL(live_intro), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(live_intro), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(live_intro), "dim");
    g->live_button = gtk_button_new_with_label("Read with privileges");
    gtk_widget_set_tooltip_text(g->live_button,
                                "Asks the system for authorisation and reads the live state");
    g_signal_connect(g->live_button, "clicked", G_CALLBACK(on_live_clicked), g);
    GtkWidget* live_cmd = make_command_row(&g->live_label);
    gtk_box_pack_start(GTK_BOX(g->live_row), live_intro, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g->live_row), g->live_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g->live_row), live_cmd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_box), g->live_row, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(g->live_row, TRUE);
    gtk_widget_set_visible(g->live_row, FALSE);

    gtk_box_pack_start(GTK_BOX(status_page), make_section("State", status_box), FALSE, FALSE, 0);

    // --- actions ---
    GtkWidget* actions = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(actions), 8);
    gtk_grid_set_row_spacing(GTK_GRID(actions), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(actions), TRUE);

    g->btn_flash = gtk_button_new_with_label("1. Load SDR image...");
    gtk_widget_set_tooltip_text(g->btn_flash,
                                "Step 1: writes the firmware partition with the chosen image "
                                "(a reboot is needed afterwards)");
    g_signal_connect(g->btn_flash, "clicked", G_CALLBACK(on_flash_clicked), g);
    gtk_grid_attach(GTK_GRID(actions), g->btn_flash, 0, 0, 2, 1);

    g->btn_activate = gtk_button_new_with_label("2. Start reception");
    gtk_widget_set_tooltip_text(g->btn_activate,
                                "Step 2: enters SDR mode if needed and runs the activation "
                                "steps (a check after each one)");
    gtk_grid_attach(GTK_GRID(actions), g->btn_activate, 0, 1, 2, 1);

    g->btn_app = gtk_button_new_with_label("Open the SDR app");
    gtk_widget_set_tooltip_text(g->btn_app,
                                "Opens the waterfall window (fmiq-app) in this session");
    g_signal_connect(g->btn_app, "clicked", G_CALLBACK(on_app_clicked), g);
    gtk_grid_attach(GTK_GRID(actions), g->btn_app, 0, 2, 2, 1);

    GtkWidget* advanced = gtk_label_new("Advanced");
    gtk_label_set_xalign(GTK_LABEL(advanced), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(advanced), "dim");
    gtk_widget_set_margin_top(advanced, 6);
    gtk_grid_attach(GTK_GRID(actions), advanced, 0, 3, 2, 1);

    g->btn_enter = gtk_button_new_with_label("Enter SDR mode only");
    gtk_widget_set_tooltip_text(g->btn_enter,
                                "Only stops the modem and enters the mode; does not start "
                                "the receiver. Not needed in the normal flow: Start reception "
                                "does it when required");
    gtk_grid_attach(GTK_GRID(actions), g->btn_enter, 0, 4, 2, 1);

    g->btn_deactivate = gtk_button_new_with_label("Stop reception");
    gtk_widget_set_tooltip_text(g->btn_deactivate,
                                "Unloads SDR mode: stops the reception loops");
    gtk_grid_attach(GTK_GRID(actions), g->btn_deactivate, 0, 5, 2, 1);

    g->btn_restore = gtk_button_new_with_label("Restore to stock...");
    gtk_widget_set_tooltip_text(g->btn_restore,
                                "Returns the phone to the stock firmware and restores the radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(g->btn_restore),
                                "destructive-action");
    gtk_widget_set_margin_top(g->btn_restore, 4);
    gtk_grid_attach(GTK_GRID(actions), g->btn_restore, 0, 6, 2, 1);

    GtkWidget* legend = gtk_label_new(
        "The normal way in is the two numbered steps, top to bottom. Loading writes the "
        "firmware partition once; starting reception does not write anything. To go back, "
        "stop reception and then restore to stock.");
    gtk_label_set_xalign(GTK_LABEL(legend), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(legend), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(legend), 36);
    gtk_style_context_add_class(gtk_widget_get_style_context(legend), "legend");

    GtkWidget* actions_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(actions_box), actions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions_box), legend, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ops_page), make_section("Actions", actions_box), FALSE, FALSE, 0);

    // stable pairs for the actions without an image
    auto* pair_enter = new std::pair<Gui*, fmiq::Action>(g, fmiq::Action::EnterMode);
    auto* pair_activate = new std::pair<Gui*, fmiq::Action>(g, fmiq::Action::Activate);
    auto* pair_deactivate = new std::pair<Gui*, fmiq::Action>(g, fmiq::Action::Deactivate);
    auto* pair_restore = new std::pair<Gui*, fmiq::Action>(g, fmiq::Action::Restore);
    g_signal_connect(g->btn_enter, "clicked", G_CALLBACK(on_simple_action), pair_enter);
    g_signal_connect(g->btn_activate, "clicked", G_CALLBACK(on_simple_action), pair_activate);
    g_signal_connect(g->btn_deactivate, "clicked", G_CALLBACK(on_simple_action), pair_deactivate);
    g_signal_connect(g->btn_restore, "clicked", G_CALLBACK(on_simple_action), pair_restore);
    g_object_set_data_full(G_OBJECT(g->btn_enter), "pair", pair_enter, [](gpointer p) {
        delete static_cast<std::pair<Gui*, fmiq::Action>*>(p);
    });
    g_object_set_data_full(G_OBJECT(g->btn_activate), "pair", pair_activate, [](gpointer p) {
        delete static_cast<std::pair<Gui*, fmiq::Action>*>(p);
    });
    g_object_set_data_full(G_OBJECT(g->btn_deactivate), "pair", pair_deactivate, [](gpointer p) {
        delete static_cast<std::pair<Gui*, fmiq::Action>*>(p);
    });
    g_object_set_data_full(G_OBJECT(g->btn_restore), "pair", pair_restore, [](gpointer p) {
        delete static_cast<std::pair<Gui*, fmiq::Action>*>(p);
    });

    // --- result ---
    g->banner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(g->banner), "banner-info");
    g->banner_headline = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g->banner_headline), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g->banner_headline), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g->banner_headline), "headline");
    g->banner_detail = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g->banner_detail), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g->banner_detail), TRUE);
    gtk_label_set_selectable(GTK_LABEL(g->banner_detail), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(g->banner_detail), 36);

    g->cmd_row = make_command_row(&g->cmd_label);
    gtk_box_pack_start(GTK_BOX(g->banner), g->banner_headline, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g->banner), g->banner_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g->banner), g->cmd_row, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(g->banner, TRUE);
    gtk_widget_set_no_show_all(g->cmd_row, TRUE);
    gtk_widget_set_visible(g->banner, FALSE);
    gtk_widget_set_visible(g->cmd_row, FALSE);
    // The banner is outside the stack so that a result, or a "not run", is visible
    // from either tab.
    gtk_box_pack_start(GTK_BOX(content), g->banner, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(content), g->banner, 1);

    // --- progress ---
    g->progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(g->progress), FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g->progress), 0.0);
    g->progress_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(g->progress_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g->progress_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(g->progress_label), 38);
    gtk_style_context_add_class(gtk_widget_get_style_context(g->progress_label), "dim");
    gtk_box_pack_start(GTK_BOX(ops_page), g->progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ops_page), g->progress_label, FALSE, FALSE, 0);

    // --- log ---
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 120);
    g->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(g->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(g->log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g->log_view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), g->log_view);
    g->log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g->log_view));
    g->tag_err = gtk_text_buffer_create_tag(g->log_buf, "err", "foreground", "#d9534f", nullptr);
    g->tag_step = gtk_text_buffer_create_tag(g->log_buf, "step", "weight", PANGO_WEIGHT_BOLD,
                                             nullptr);

    GtkWidget* log_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(log_box), scroll, TRUE, TRUE, 0);
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget* log_frame = make_section("sdr-load log", log_box);
    gtk_box_pack_start(GTK_BOX(ops_page), log_frame, TRUE, TRUE, 0);

    gtk_stack_add_titled(GTK_STACK(g->stack), status_page, "state", "State");
    gtk_stack_add_titled(GTK_STACK(g->stack), ops_page, "operations", "Operations");
    // The window opens on the steps, with the guide on top: the state is one tap away and
    // is only needed to diagnose.
    gtk_stack_set_visible_child_name(GTK_STACK(g->stack), "operations");

    // --- footer ---
    g->tool_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g->tool_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g->tool_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(g->tool_label), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g->tool_label), "dim");
    gtk_box_pack_start(GTK_BOX(content), g->tool_label, FALSE, FALSE, 0);

    g_signal_connect(g->window, "delete-event", G_CALLBACK(on_delete_event), g);
}

void on_activate(GtkApplication* app, gpointer data) {
    Gui* g = static_cast<Gui*>(data);
    if (g->window) {
        gtk_window_present(GTK_WINDOW(g->window));
        return;
    }
    resolve_privileges(g);
    install_theme();
    build_window(g, app);
    gtk_widget_show_all(g->window);
    resolve_tool(g);
    update_tool_caption(g);
    set_buttons_enabled(g);
    start_status(g, false);
}

void usage() {
    std::printf(
        "%s -- load, activation and revert of SDR mode, with sdr-load as a subprocess\n"
        "\n"
        "usage:\n"
        "  furimodem-sdr              open the window\n"
        "  furimodem-sdr --version    version\n"
        "  furimodem-sdr --help       this help\n"
        "\n"
        "environment variable:\n"
        "  SDR_LOAD=<path>            tool to invoke (by default, the one next to this\n"
        "                             binary or the one in PATH)\n"
        "\n"
        "Privileges: operations that write to the phone run through pkexec, so the system\n"
        "asks for authorisation in its own dialogue; the window never sees a password.\n"
        "Without pkexec, or if the dialogue is cancelled, the exact command is shown with\n"
        "a button to copy it into a terminal.\n",
        kAppVersion);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-V") == 0) {
            std::printf("%s\n", kAppVersion);
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
    }

    Gui gui;
    GtkApplication* app = gtk_application_new("org.furimodem.sdrgui",
                                              static_cast<GApplicationFlags>(0));
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &gui);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
