#include "status.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <vector>

namespace fmiq {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const char* p) {
    return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

// "standard-a (image table), image verified by sha256" ->
//   name = standard-a, gloss = image verified by sha256
// "UNKNOWN (reason)" -> name = UNKNOWN, gloss = reason
void split_profile(const std::string& value, StatusInfo* out) {
    const std::string v = trim(value);
    if (v.empty()) return;
    if (starts_with(v, "UNKNOWN") || starts_with(v, "unknown")) {
        out->profile = "UNKNOWN";
        out->profile_known = false;
        size_t l = v.find('(');
        size_t r = v.rfind(')');
        if (l != std::string::npos) {
            out->profile_extra = trim(v.substr(l + 1, (r != std::string::npos && r > l) ? r - l - 1
                                                                                       : std::string::npos));
        }
        return;
    }
    out->profile_known = true;
    const std::string sep = "), ";
    size_t pos = v.find(sep);
    if (pos != std::string::npos) {
        out->profile = trim(v.substr(0, pos));
        out->profile_extra = trim(v.substr(pos + sep.size()));
    } else {
        size_t paren = v.find(" (");
        out->profile = paren == std::string::npos ? v : trim(v.substr(0, paren));
    }
    // Drop the opening parenthesis left in the name.
    size_t sp = out->profile.find(" (");
    if (sp != std::string::npos) out->profile = out->profile.substr(0, sp);
}

// "loops=armed, rails=on"
void split_activation(const std::string& value, StatusInfo* out) {
    std::stringstream ss(value);
    std::string part;
    while (std::getline(ss, part, ',')) {
        size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(part.substr(0, eq));
        std::string v = trim(part.substr(eq + 1));
        if (k == "loops") out->loops = v;
        else if (k == "rails") out->rails = v;
    }
}

std::string ring_state(const std::string& value) {
    std::string v = trim(value);
    if (v.empty()) return v;
    size_t pos = v.rfind("->");
    std::string tail = pos == std::string::npos ? v : trim(v.substr(pos + 2));
    if (tail == "LATE") return "late";
    if (tail == "QUIET") return "quiet";
    return tail;
}

}  // namespace

StatusInfo parse_status(const std::string& text) {
    StatusInfo s;
    s.raw = text;

    std::map<std::string, std::string> base;
    std::map<std::string, std::string> mode;
    std::map<std::string, std::string>* section = &base;
    bool in_mode = false;

    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        if (indent == 0) continue;  // "sdr-load 1.1 --status" and little else

        std::string body = line.substr(indent);
        size_t colon = body.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(body.substr(0, colon));
        std::string value = trim(body.substr(colon + 1));
        if (key.empty()) continue;

        if (key == "SDR mode") {
            in_mode = true;
            section = &mode;
            continue;
        }
        // The "SDR mode" block is indented at 8; its keys too. A key at 6 after
        // that block (for example "next     :") goes back to the base block.
        if (in_mode) {
            section = indent <= 6 ? &base : &mode;
        } else if (indent >= 8) {
            // No recognised header: deeper indentation is kept apart.
            section = &mode;
        }
        (*section)[key] = value;
    }

    auto pick = [&](const char* key) -> std::string {
        auto it = mode.find(key);
        if (it != mode.end() && !it->second.empty()) return it->second;
        it = base.find(key);
        return it != base.end() ? it->second : std::string();
    };

    s.state = base["state"];
    s.pending = base["pending"];
    s.updated = base["updated"];
    s.observed = base["observed"];
    s.radios = base["radios"];
    s.last_error = base["last error"];
    s.hint = base["next"];

    s.image = pick("image");
    s.sha256 = pick("sha256");
    // What the partition holds, as the tool hashed it: the state file is not used for
    // this. The keys come from the "partition / image now / image check" block.
    s.partition = base["partition"];
    s.image_now = base["image now"];
    s.image_size = base["image size"];
    s.image_kind = base["image kind"];
    s.image_check = base["image check"];
    s.image_incoherent = starts_with(s.image_check, "INCOHERENT");
    s.image_unverified = starts_with(s.image_check, "unverified");
    s.image_ok = starts_with(s.image_check, "OK");
    s.pll = mode["PLL"];
    s.ring = ring_state(mode["ring"]);
    split_profile(pick("profile"), &s);
    s.scan = s.profile_extra.find("sweep image") != std::string::npos ||
             s.observed.find("sweep image") != std::string::npos;
    split_activation(base["activation"], &s);

    s.parsed = !base.empty() || !mode.empty();
    s.observable = !s.observed.empty() && !starts_with(s.observed, "undetermined");
    s.sdr_active = starts_with(s.observed, "SDR active");
    if (!s.observable && s.observed.empty() && !mode.empty()) s.observable = true;
    return s;
}

std::string mode_text(const StatusInfo& s) {
    if (!s.observable) return "Undetermined (the modem cannot be observed)";
    // A sweep image keeps the modem in service: the capture comes from the scanner, so it
    // is not a "no dump" failure when the ring is quiet, it is simply "no scan yet".
    if (s.observed.find("sweep image") != std::string::npos)
        return s.sdr_active ? "SDR mode loaded (sweep image)" : "Undetermined (sweep image)";
    if (!s.sdr_active) return "In service (standard mode)";
    if (s.ring == "late") return "SDR mode active, with dump";
    return "SDR mode active, no dump";
}

}  // namespace fmiq
