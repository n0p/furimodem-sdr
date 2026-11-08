// How the window gets privileges: direct when it is root, behind polkit when it
// is not.
//
// The window never handles passwords and never builds a shell command to elevate
// itself: it asks `pkexec` to run the console tool, and the system's polkit agent
// draws the dialogue. Everything here is a pure decision over strings so it can be
// tested without GTK, without polkit and without the phone.
#pragma once

#include <string>
#include <vector>

namespace fmiq {

// What will be done with a command that needs administrator rights.
enum class Route {
    NotNeeded,  // the action runs as the user, as is
    Direct,     // the window is already root: the tool is run directly
    Pkexec,     // not root, pkexec available: the tool is run through polkit
    NoPkexec,   // not root and no pkexec: the command is shown, not run
};

struct Plan {
    Route route = Route::NotNeeded;
    std::vector<std::string> argv;  // exactly what will be executed
};

// Why pkexec did not run the tool. 0 in every other case (the tool ran and its
// own exit code is the one that matters).
enum class AuthFailure {
    None,
    Cancelled,    // the dialogue was dismissed or the authorisation was refused
    Unavailable,  // pkexec could not authorise at all (no agent, bad policy, ...)
};

struct AuthText {
    std::string headline;
    std::string detail;
};

// Absolute path of an executable. `name` may be a path or a bare name looked up in
// `path_env` (empty: the PATH of the process). Empty when it is not executable.
std::string find_in_path(const std::string& name, const std::string& path_env = std::string());

// The same, made absolute through the current directory when PATH has relative
// entries: pkexec only accepts a program with a slash in it.
std::string absolute_tool(const std::string& tool, const std::string& path_env = std::string());

// Decides how `tool_argv` is run. `needs_root` comes from the action spec, `is_root`
// from geteuid(), `pkexec_path` from find_in_path("pkexec").
Plan plan_run(bool needs_root, const std::vector<std::string>& tool_argv, bool is_root,
              const std::string& pkexec_path, const std::string& path_env = std::string());

// The command as a person would type it: the pkexec form when that is the route,
// `sudo ...` when pkexec is not installed. Never used to execute anything.
std::string copyable_command(const Plan& plan, const std::vector<std::string>& tool_argv);

// pkexec's own exit codes, which are not the tool's: 126 when the authorisation was
// not obtained, 127 when it could not even try.
AuthFailure classify_pkexec_exit(int code);

AuthText auth_failure_text(AuthFailure failure);

}  // namespace fmiq
