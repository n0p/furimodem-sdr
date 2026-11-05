#include "actions.hpp"

#include <cstdio>

namespace fmiq {
namespace {

const ActionSpec kStatus{
    Action::Status, "Refresh state", "", "", false, false, false, false};

const ActionSpec kFlashImage{
    Action::FlashImage,
    "Load image...",
    "Load the SDR mode image?",
    "The phone's firmware partition will be written with the chosen image, checking "
    "its sha256 before and after. This is the only step that changes the firmware. "
    "THE PHONE WILL RESTART when it finishes: any call in progress will be dropped "
    "and mobile service will be gone until you restore to stock.",
    true,
    true,
    true,
    true};

const ActionSpec kEnterMode{
    Action::EnterMode,
    "Enter SDR mode",
    "Enter SDR mode?",
    "The modem will stop working as a radio and the phone will be left without "
    "cellular service until you restore to stock. No partition is written.",
    true,
    true,
    false,
    false};

const ActionSpec kActivate{
    Action::Activate,
    "Start reception",
    "Start reception?",
    "The nine activation steps run (profile, PLL, rails and loops) with a check "
    "after each one. No partition is written.",
    true,
    true,
    false,
    false};

const ActionSpec kDeactivate{
    Action::Deactivate,
    "Stop reception",
    "Stop reception?",
    "The reception loops are stopped. The rails stay powered: the tool does not "
    "switch them off in software. No partition is written.",
    true,
    true,
    false,
    false};

const ActionSpec kRestore{
    Action::Restore,
    "Restore to stock...",
    "Go back to the normal phone?",
    "SDR mode is left, the radios are given back and the modem is waited for. "
    "If it does not come back, the tool writes the recovery image and THE PHONE "
    "WILL RESTART.",
    true,
    true,
    true,
    true};

// Quoting for displaying a command, not for passing it to a shell.
std::string quote_arg(const std::string& a) {
    if (!a.empty()) {
        bool safe = true;
        for (char c : a) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
                            c == '/' || c == ':' || c == '=' || c == '+' || c == ',' ||
                            c == '@';
            if (!ok) {
                safe = false;
                break;
            }
        }
        if (safe) return a;
    }
    std::string out = "'";
    for (char c : a) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

}  // namespace

const ActionSpec& spec_for(Action a) {
    switch (a) {
        case Action::Status: return kStatus;
        case Action::FlashImage: return kFlashImage;
        case Action::EnterMode: return kEnterMode;
        case Action::Activate: return kActivate;
        case Action::Deactivate: return kDeactivate;
        case Action::Restore: return kRestore;
    }
    return kStatus;
}

const char* action_id(Action a) {
    switch (a) {
        case Action::Status: return "status";
        case Action::FlashImage: return "flash";
        case Action::EnterMode: return "enter";
        case Action::Activate: return "activate";
        case Action::Deactivate: return "deactivate";
        case Action::Restore: return "restore";
    }
    return "status";
}

std::vector<std::string> argv_for(Action a, const std::string& tool,
                                  const std::string& image) {
    std::vector<std::string> v;
    v.push_back(tool);
    switch (a) {
        case Action::Status:
            v.push_back("--status");
            break;
        case Action::FlashImage:
            v.push_back("--image");
            v.push_back(image);
            // The window has already asked; without a tty, sdr-load cannot ask.
            v.push_back("--yes");
            // The reboot is decided by the user, not by the window.
            v.push_back("--no-reboot");
            break;
        case Action::EnterMode:
            v.push_back("--no-flash");
            v.push_back("--yes");
            break;
        case Action::Activate:
            v.push_back("--activate");
            v.push_back("--yes");
            break;
        case Action::Deactivate:
            v.push_back("--deactivate");
            v.push_back("--yes");
            break;
        case Action::Restore:
            v.push_back("--restore");
            v.push_back("--yes");
            v.push_back("--no-reboot");
            break;
    }
    return v;
}

std::string command_text(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& a : argv) {
        if (!out.empty()) out += " ";
        out += quote_arg(a);
    }
    return out;
}

ExitInfo describe_exit(Action a, int code) {
    ExitInfo e;

    if (code >= 128) {
        e.headline = "The process ended on a signal";
        e.detail = "Code " + std::to_string(code) +
                   ". It may have been the system or a forced close; check the log and "
                   "refresh the state before repeating.";
        return e;
    }

    switch (code) {
        case 0:
            e.ok = true;
            if (a == Action::Status) {
                e.headline = "State read";
                e.detail = "The tool answered without errors.";
            } else if (a == Action::Deactivate) {
                e.headline = "Reception stopped";
                e.detail = "The loops are stopped. SDR mode is still loaded; the rails "
                           "stay powered.";
            } else if (a == Action::Activate) {
                e.headline = "Reception active";
                e.detail = "All nine steps finished and were verified.";
            } else if (a == Action::EnterMode) {
                e.headline = "SDR mode loaded";
                e.detail = "The modem is in SDR mode. Reception is started separately.";
            } else if (a == Action::Restore) {
                e.headline = "Phone restored";
                e.detail = "SDR mode has been left and the radios are back in place.";
            } else {
                e.headline = "Operation completed";
            }
            return e;

        case 1:
            e.headline = "Bad usage";
            e.detail = "The tool rejected the arguments. This is a bug in this window: "
                       "the exact command is in the log.";
            return e;

        case 2:
            e.headline = "A precondition is missing";
            e.detail = "Privileges, a device node or the modem being ready: the log says "
                       "which. The operation was not completed.";
            return e;

        case 3:
            e.headline = "The radios could not be blocked";
            e.detail = "WiFi or Bluetooth are still on. Turn them off and repeat the "
                       "operation.";
            return e;

        case 4:
            e.headline = "The flash failed";
            e.detail = "The partition may have been left half written. Repeat the load or "
                       "restore to stock before using the phone normally.";
            return e;

        case 5:
            e.headline = "The sha256 does not match";
            e.detail = "The partition does not return the expected image. The tool does "
                       "not accept it; it did not go on.";
            return e;

        case 6:
            if (a == Action::FlashImage) {
                e.ok = true;
                e.reboot = true;
                e.headline = "Image written and verified";
                e.detail = "A reboot is needed for the modem to use the new image. After "
                           "that, enter SDR mode.";
            } else if (a == Action::Restore) {
                e.reboot = true;
                e.headline = "Recovery image written; reboot needed";
                e.detail = "The tool had to write the recovery image. Reboot and check "
                           "the state.";
            } else {
                e.reboot = true;
                e.headline = "A reboot is needed";
                e.detail = "A flash is pending reboot. Reboot before entering SDR mode.";
            }
            return e;

        case 7:
            e.headline = "The SDR mode ioctl failed";
            e.detail = "The modem may be busy. Reboot the phone and repeat.";
            return e;

        case 8:
            e.headline = "Mode not verified";
            e.detail = "The link rings are still moving: the mode was not accepted as "
                       "good.";
            return e;

        case 9:
            e.headline = "Incomplete revert";
            e.detail = "The phone did not come all the way back. Repeat the restore to "
                       "stock; if it stays the same, reboot and check the state.";
            return e;

        case 10:
            e.reboot = true;
            e.headline = "Recovery image written; reboot needed";
            e.detail = "The modem did not come back and the good image was written. "
                       "Reboot and check the state.";
            return e;

        case 11:
            e.ok = true;
            e.reboot = true;
            e.headline = "Rebooting the phone";
            e.detail = "The tool started the reboot as a last resort.";
            return e;

        case 12:
            e.headline = "Mode active, no dump programmed";
            e.detail = "The dump engine mark is missing. Repeat the activation.";
            return e;

        case 13:
            e.headline = "Image without a mode profile";
            e.detail = "The partition is not in the tool's profile table, so activation "
                       "was refused without requesting any mode. Update sdr-load or pass "
                       "a profile with --profile.";
            return e;

        case 14:
            e.headline = "Activation failed";
            e.detail = "The sequence was disarmed before completing. The loops may still "
                       "be armed: check the log and repeat, stop reception or restore to "
                       "stock.";
            return e;

        default:
            e.headline = "Finished with an unexpected code";
            e.detail = "Code " + std::to_string(code) +
                       ". Check the full log before repeating the operation.";
            return e;
    }
}

}  // namespace fmiq
