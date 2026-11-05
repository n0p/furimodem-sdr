// Tests of the privilege plan: how a command is run, and what is shown as a fallback.
// They need neither GTK, nor polkit, nor the phone: they are built and run on the
// laptop and only look at strings.
#include <cstdio>
#include <string>
#include <vector>

#include "actions.hpp"
#include "privileges.hpp"

namespace {

int g_checks = 0;
bool g_failed = false;

void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        g_failed = true;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

bool has(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

// A PATH with one known executable and one that does not exist.
const char* kPath = "/usr/bin:/bin";

void test_find_in_path() {
    CHECK(fmiq::find_in_path("sh", kPath) == "/bin/sh" ||
          fmiq::find_in_path("sh", kPath) == "/usr/bin/sh");
    CHECK(fmiq::find_in_path("furimodem-does-not-exist", kPath).empty());
    CHECK(fmiq::find_in_path("", kPath).empty());
    // A path with a slash is used as it is, without looking at PATH.
    CHECK(fmiq::find_in_path("/bin/sh", "/nowhere") == "/bin/sh");
    CHECK(fmiq::find_in_path("/nowhere/sh", "").empty());
    // Relative entries are searched from the current directory.
    CHECK(fmiq::find_in_path("sh", "") == "" || has(fmiq::find_in_path("sh", ""), "sh"));
}

void test_plan_status() {
    const auto argv = fmiq::argv_for(fmiq::Action::Status, "/usr/bin/sdr-load");
    const auto plan = fmiq::plan_run(false, argv, false, "/usr/bin/pkexec", kPath);
    CHECK(plan.route == fmiq::Route::NotNeeded);
    CHECK(plan.argv == argv);
    CHECK(fmiq::copyable_command(plan, argv) == "/usr/bin/sdr-load --status");
}

void test_plan_root() {
    const auto argv = fmiq::argv_for(fmiq::Action::EnterMode, "/usr/bin/sdr-load");
    const auto plan = fmiq::plan_run(true, argv, true, "/usr/bin/pkexec", kPath);
    CHECK(plan.route == fmiq::Route::Direct);
    CHECK(plan.argv == argv);
    CHECK(plan.argv.front() == "/usr/bin/sdr-load");
    CHECK(fmiq::copyable_command(plan, argv) == "/usr/bin/sdr-load --no-flash --yes");
}

void test_plan_pkexec() {
    const auto argv = fmiq::argv_for(fmiq::Action::FlashImage, "/usr/bin/sdr-load", "/tmp/a b.img");
    const auto plan = fmiq::plan_run(true, argv, false, "/usr/bin/pkexec", kPath);
    CHECK(plan.route == fmiq::Route::Pkexec);
    // pkexec first, then the tool, then its arguments untouched: no shell, no quoting.
    CHECK(plan.argv.size() == argv.size() + 1);
    CHECK(plan.argv[0] == "/usr/bin/pkexec");
    CHECK(plan.argv[1] == "/usr/bin/sdr-load");
    CHECK(plan.argv[2] == "--image");
    CHECK(plan.argv[3] == "/tmp/a b.img");
    CHECK(plan.argv[4] == "--yes");
    CHECK(plan.argv[5] == "--no-reboot");

    // The copyable form quotes for the terminal, but what is executed does not.
    const std::string text = fmiq::copyable_command(plan, argv);
    CHECK(text == "/usr/bin/pkexec /usr/bin/sdr-load --image '/tmp/a b.img' --yes --no-reboot");
    CHECK(has(text, "pkexec"));
    CHECK(!has(text, "sudo"));
}

void test_plan_bare_tool_name() {
    // A bare name would be looked up in pkexec's own PATH: it is made absolute first.
    // The name must not exist in kPath, or this test would depend on what the host has
    // installed (a stand-in sdr-load in /usr/bin made it fail once).
    const auto argv = fmiq::argv_for(fmiq::Action::Activate, "sdr-load-not-installed");
    const auto plan = fmiq::plan_run(true, argv, false, "/usr/bin/pkexec", kPath);
    CHECK(plan.route == fmiq::Route::Pkexec);
    // Not in this PATH, so the name is left as it is rather than invented.
    CHECK(plan.argv[1] == "sdr-load-not-installed");

    const auto found = fmiq::plan_run(true, fmiq::argv_for(fmiq::Action::Activate, "sh"), false,
                                      "/usr/bin/pkexec", kPath);
    CHECK(found.route == fmiq::Route::Pkexec);
    CHECK(found.argv[1][0] == '/');
    CHECK(has(found.argv[1], "sh"));
}

void test_plan_without_pkexec() {
    const auto argv = fmiq::argv_for(fmiq::Action::Restore, "/usr/bin/sdr-load");
    const auto plan = fmiq::plan_run(true, argv, false, std::string(), kPath);
    CHECK(plan.route == fmiq::Route::NoPkexec);
    // Nothing is invented: the command is the tool's own, with the sudo prefix in the
    // text that is offered to the user.
    CHECK(plan.argv == argv);
    CHECK(fmiq::copyable_command(plan, argv) == "sudo /usr/bin/sdr-load --restore --yes --no-reboot");
}

void test_every_action_keeps_its_arguments() {
    const std::vector<fmiq::Action> actions = {
        fmiq::Action::Status,     fmiq::Action::FlashImage, fmiq::Action::EnterMode,
        fmiq::Action::Activate,   fmiq::Action::Deactivate, fmiq::Action::Restore};
    for (fmiq::Action a : actions) {
        const auto argv = fmiq::argv_for(a, "/usr/bin/sdr-load", "/tmp/x.img");
        const auto plan = fmiq::plan_run(true, argv, false, "/usr/bin/pkexec", kPath);
        CHECK(plan.argv.size() == argv.size() + 1);
        for (size_t i = 1; i < argv.size(); ++i) CHECK(plan.argv[i + 1] == argv[i]);
        CHECK(!has(fmiq::copyable_command(plan, argv), "sudo"));
    }
}

void test_pkexec_exit_codes() {
    CHECK(fmiq::classify_pkexec_exit(126) == fmiq::AuthFailure::Cancelled);
    CHECK(fmiq::classify_pkexec_exit(127) == fmiq::AuthFailure::Unavailable);
    // The tool's own codes are not pkexec's: they must go through untouched.
    for (int code : {0, 1, 2, 13, 14, 6, 10, 128 + 9}) {
        CHECK(fmiq::classify_pkexec_exit(code) == fmiq::AuthFailure::None);
    }

    const fmiq::AuthText cancelled = fmiq::auth_failure_text(fmiq::AuthFailure::Cancelled);
    CHECK(has(cancelled.headline, "cancelled"));
    CHECK(has(cancelled.detail, "nothing was run") || has(cancelled.detail, "Nothing was run"));

    const fmiq::AuthText refused = fmiq::auth_failure_text(fmiq::AuthFailure::Unavailable);
    CHECK(!refused.headline.empty());
    CHECK(has(refused.detail, "terminal"));

    const fmiq::AuthText none = fmiq::auth_failure_text(fmiq::AuthFailure::None);
    CHECK(none.headline.empty());
    CHECK(none.detail.empty());
}

}  // namespace

int main() {
    test_find_in_path();
    test_plan_status();
    test_plan_root();
    test_plan_pkexec();
    test_plan_bare_tool_name();
    test_plan_without_pkexec();
    test_every_action_keeps_its_arguments();
    test_pkexec_exit_codes();

    if (g_failed) {
        std::fprintf(stderr, "%d checks, with failures\n", g_checks);
        return 1;
    }
    std::printf("%d checks OK\n", g_checks);
    return 0;
}
