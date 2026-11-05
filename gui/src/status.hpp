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
    std::string image;         // partition and usable size
    std::string sha256;
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

    bool load_error = false;   // the tool could not be run or read
    std::string error;
};

// Interprets the full output of `sdr-load --status`. Never throws.
StatusInfo parse_status(const std::string& text);

// Short phrase for the "Mode" row.
std::string mode_text(const StatusInfo& s);

}  // namespace fmiq
