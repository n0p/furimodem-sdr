#include "privileges.hpp"

#include <cstdlib>
#include <limits.h>

#include <unistd.h>

#include "actions.hpp"

namespace fmiq {
namespace {

std::string env_path() {
    const char* p = std::getenv("PATH");
    return p ? std::string(p) : std::string();
}

std::string absolute(const std::string& path) {
    if (path.empty() || path[0] == '/') return path;
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved)) return std::string(resolved);
    char cwd[PATH_MAX];
    if (::getcwd(cwd, sizeof(cwd))) {
        std::string out(cwd);
        if (out.empty() || out.back() != '/') out += '/';
        return out + path;
    }
    return path;
}

}  // namespace

std::string find_in_path(const std::string& name, const std::string& path_env) {
    if (name.empty()) return std::string();

    if (name.find('/') != std::string::npos) {
        return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
    }

    const std::string paths = path_env.empty() ? env_path() : path_env;
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t colon = paths.find(':', start);
        std::string dir = paths.substr(
            start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) dir = ".";
        const std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return std::string();
}

std::string absolute_tool(const std::string& tool, const std::string& path_env) {
    const std::string found = find_in_path(tool, path_env);
    if (found.empty()) return tool;  // left as it is: the caller still shows it
    return absolute(found);
}

Plan plan_run(bool needs_root, const std::vector<std::string>& tool_argv, bool is_root,
              const std::string& pkexec_path, const std::string& path_env) {
    Plan plan;

    if (!needs_root) {
        plan.route = Route::NotNeeded;
        plan.argv = tool_argv;
        return plan;
    }
    if (is_root) {
        plan.route = Route::Direct;
        plan.argv = tool_argv;
        return plan;
    }
    if (pkexec_path.empty() || tool_argv.empty()) {
        plan.route = Route::NoPkexec;
        plan.argv = tool_argv;
        return plan;
    }

    plan.route = Route::Pkexec;
    plan.argv.reserve(tool_argv.size() + 1);
    plan.argv.push_back(pkexec_path);
    // pkexec runs the program itself, not a shell: the tool goes first and its
    // arguments follow untouched, one by one.
    plan.argv.push_back(absolute_tool(tool_argv.front(), path_env));
    for (size_t i = 1; i < tool_argv.size(); ++i) plan.argv.push_back(tool_argv[i]);
    return plan;
}

std::string copyable_command(const Plan& plan, const std::vector<std::string>& tool_argv) {
    if (plan.route == Route::Pkexec && !plan.argv.empty()) return command_text(plan.argv);
    if (plan.route == Route::NotNeeded) return command_text(tool_argv);
    if (plan.route == Route::Direct) return command_text(tool_argv);
    return "sudo " + command_text(tool_argv);
}

AuthFailure classify_pkexec_exit(int code) {
    switch (code) {
        case 126: return AuthFailure::Cancelled;
        case 127: return AuthFailure::Unavailable;
        default: return AuthFailure::None;
    }
}

AuthText auth_failure_text(AuthFailure failure) {
    AuthText t;
    switch (failure) {
        case AuthFailure::Cancelled:
            t.headline = "Authorisation cancelled";
            t.detail =
                "No privileges were granted, so nothing was run and the phone was not "
                "touched. Repeat the operation and accept the system dialogue, or copy "
                "the command below and run it in a terminal.";
            break;
        case AuthFailure::Unavailable:
            t.headline = "The system could not authorise the command";
            t.detail =
                "pkexec refused to run the tool: there may be no polkit agent in this "
                "session, or the installed policy does not cover this program. Nothing "
                "was run. Copy the command below and run it in a terminal.";
            break;
        case AuthFailure::None: break;
    }
    return t;
}

}  // namespace fmiq
