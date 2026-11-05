// Why there are no samples from the receiver.
//
// The module used to answer every failure with SOAPY_SDR_TIMEOUT: a missing node, an unmapped
// window, a mapped window where the firmware publishes nothing and a block with an unknown
// signature all looked the same from the app ("the ring is not publishing"). The driver now
// exposes readSetting("rx_ring") with the state of the ring (the producer is in
// driver/SoapyFurimodemIQ.cpp), and these are the three messages the app shows, one per cause.
//
// They are short on purpose: the amber band is two lines at scale 2 (about 38 characters per
// line on a 480 px screen). The detail the module adds after ": " (node, errno, signature) goes
// to app.log, not to the screen.
#pragma once

#include <string>

namespace fmiq {

enum class RingState {
    Ok,            // mapped and delivering blocks with a known signature
    NotOpened,     // setupStream(RX) has not run yet
    NoDevice,      // the memory window does not exist (patched image not loaded?)
    NoPermission,  // it exists but cannot be opened by this user
    NoMap,         // open() succeeded but the window could not be mapped
    NoLayout,      // mapped, but the ring geometry does not fit in the window
    NoBlocks,      // mapped, but no complete block has been published (no sweep)
    NoFormat,      // blocks published with a signature this driver does not know
    Unknown,
};

// Parses the value of readSetting("rx_ring"): "ok", "no-device", "no-permission", "no-map",
// "no-layout", "no-blocks", "no-format" or "not-opened", optionally followed by ": detail".
// Anything else is Unknown (an older module that does not implement the setting).
inline RingState ring_state_from_setting(const std::string& v) {
    const std::string key = v.substr(0, v.find(':'));
    if (key == "ok") return RingState::Ok;
    if (key == "not-opened") return RingState::NotOpened;
    if (key == "no-device") return RingState::NoDevice;
    if (key == "no-permission") return RingState::NoPermission;
    if (key == "no-map") return RingState::NoMap;
    if (key == "no-layout") return RingState::NoLayout;
    if (key == "no-blocks") return RingState::NoBlocks;
    if (key == "no-format") return RingState::NoFormat;
    return RingState::Unknown;
}

// True when the ring is not even mapped: the app then says why.
inline bool ring_not_mapped(RingState s) {
    return s == RingState::NoDevice || s == RingState::NoPermission || s == RingState::NoMap ||
           s == RingState::NoLayout;
}

// Notice for the amber band. Empty when there is nothing to say (the ring is reading, or the
// module is too old to report). `detail` is kept for the caller's log; it is not drawn.
inline std::string ring_notice(RingState s) {
    switch (s) {
        case RingState::NoDevice:
            return "RX RING NOT MAPPED: THE MEMORY WINDOW IS MISSING (PATCHED IMAGE?)";
        case RingState::NoPermission:
            return "RX RING NOT MAPPED: NO PERMISSION FOR THE MODEM WINDOW";
        case RingState::NoMap:
            return "RX RING NOT MAPPED: THE WINDOW COULD NOT BE MAPPED";
        case RingState::NoLayout:
            return "RX RING NOT MAPPED: IT DOES NOT FIT IN THE WINDOW";
        case RingState::NoBlocks:
            return "RX RING MAPPED BUT NOTHING IS PUBLISHED: NO CAPTURE";
        case RingState::NoFormat:
            return "BLOCK SIGNATURE UNKNOWN: IMAGE AND DRIVER DISAGREE";
        case RingState::NotOpened:
        case RingState::Ok:
        case RingState::Unknown:
        default:
            return std::string();
    }
}

}  // namespace fmiq
