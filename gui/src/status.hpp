// Reading of the output of `sdr-load --status`.
//
// The utility does not talk to the modem: everything it knows comes from the console
// tool. Here only the "key : value" lines it prints are split, without interpreting
// anything it has not already said.
#pragma once

#include <string>

namespace fmiq {

struct StatusInfo {
    bool parsed = false;       // at least the state block has been recognised
    std::string raw;           // full output, for the technical detail
    std::string state;         // persisted state (sdr-active, ok, ...)
    std::string pending;
    std::string updated;
    std::string observed;      // what is seen right now
    std::string image;         // partition and usable size (from the SDR mode block)
    std::string sha256;        // sha256 of the partition (from the SDR mode block)
    std::string partition;     // the partition the image check read
    std::string image_now;     // sha256 of what the partition holds right now
    std::string image_size;    // "63018064 B usable"
    std::string image_kind;    // "stock image", "mode image standard-a", ...
    std::string image_check;   // one-line verdict: OK / INCOHERENT / unverified
    std::string profile;       // profile name
    std::string profile_extra; // gloss that goes with the profile
    std::string pll;
    std::string loops;         // armed / stopped
    std::string rails;         // on / no record
    std::string ring;          // late / quiet
    std::string last_error;
    std::string hint;
    std::string radios;

    bool profile_known = false;
    bool sdr_active = false;
    bool observable = true;
    // The state file and the partition do not describe the same image: the state is not
    // trusted, reception cannot start and the action to offer is to write the image.
    bool image_incoherent = false;
    bool image_unverified = false;
    bool image_ok = false;
    // The profile is a sweep image (scan = 1): the modem stays in service and the capture
    // comes from its scan. Entering SDR mode would stop it and leave nothing to read.
    bool scan = false;

    bool load_error = false;   // the tool could not be run or read
    std::string error;
};

// Interprets the full output of `sdr-load --status`. Never throws.
StatusInfo parse_status(const std::string& text);

// Short phrase for the "Mode" row.
std::string mode_text(const StatusInfo& s);

}  // namespace fmiq
