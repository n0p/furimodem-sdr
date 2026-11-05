// Window actions and their translation into `sdr-load` commands.
//
// The window knows nothing about the device: every button is a console tool
// command, with its exact arguments. This file concentrates that translation and
// the reading of the exit codes, so it can be tested without GTK and without a
// phone.
#pragma once

#include <string>
#include <vector>

namespace fmiq {

enum class Action {
    Status,       // --status: read only
    FlashImage,   // --image <path>: writes the firmware partition
    EnterMode,    // --no-flash: enters SDR mode (ioctl)
    Activate,     // --activate: the nine steps
    Deactivate,   // --deactivate: stops the loops
    Restore,      // --restore: goes back to the normal phone
};

struct ActionSpec {
    Action action;
    const char* title;            // button text
    const char* question;         // confirmation question (empty: do not ask)
    const char* detail;           // what it does and what it touches, plainly
    bool needs_root;              // the tool requires euid 0
    bool mutates;                 // changes something on the phone or in its state
    bool writes_partition;        // writes the firmware partition
    bool dangerous;               // destructive style in the dialog
};

const ActionSpec& spec_for(Action a);

// Exact command that is run. `image` is only used for FlashImage.
std::vector<std::string> argv_for(Action a, const std::string& tool,
                                  const std::string& image = std::string());

// The same command as the user would type it in a terminal.
std::string command_text(const std::vector<std::string>& argv);

struct ExitInfo {
    bool ok = false;      // the result is the expected one
    bool reboot = false;  // a reboot is needed (or is being done)
    std::string headline;
    std::string detail;
};

// Interprets the sdr-load exit code in the context of the action.
ExitInfo describe_exit(Action a, int code);

const char* action_id(Action a);

}  // namespace fmiq
