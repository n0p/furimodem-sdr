// iqmode: enter and leave the device's SDR mode.
//
// Precondition (checked, not assumed): no data bearer, WiFi off, Bluetooth blocked.
// Enter: SDR-mode ioctl on the control node -> the link rings stop and the working memory is
// released. Leave: exit ioctl + restore the radios.
//
// Status: the ioctl sequence is measured on hardware; the node name and the codes live only
// here, so this file is the single place that knows them.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr const char* kIoctlNode = "/dev/ccci_ioctl0";
constexpr unsigned long kEnterSdrMode = 0x430E;
constexpr unsigned long kExitSdrMode  = 0x430F;

int count_up_interfaces(const std::string& prefix) {
    std::ifstream f("/proc/net/dev");
    if (!f) return -1;
    std::string line; int n = 0;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        name.erase(0, name.find_first_not_of(" \t"));
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        // the UP flag comes after the colon
        if (line.find("UP") != std::string::npos) ++n;
    }
    return n;
}

bool wifi_blocked() {
    // rfkill: any "wlan" entry with Soft blocked: yes
    std::ifstream f("/sys/class/rfkill/rfkill0/type");
    std::string type;
    bool found_wifi = false, blocked = true;
    for (int i = 0; i < 16; ++i) {
        std::string base = "/sys/class/rfkill/rfkill" + std::to_string(i);
        std::ifstream t(base + "/type");
        if (!t) break;
        std::getline(t, type);
        if (type != "wlan") continue;
        found_wifi = true;
        std::ifstream s(base + "/soft");
        std::string v; std::getline(s, v);
        if (v != "1") blocked = false;
    }
    return found_wifi ? blocked : true;   // no WiFi radio -> treat it as blocked
}

bool bt_blocked() {
    bool found = false, blocked = true;
    for (int i = 0; i < 16; ++i) {
        std::string base = "/sys/class/rfkill/rfkill" + std::to_string(i);
        std::ifstream t(base + "/type");
        if (!t) break;
        std::string type; std::getline(t, type);
        if (type != "bluetooth") continue;
        found = true;
        std::ifstream s(base + "/soft");
        std::string v; std::getline(s, v);
        if (v != "1") blocked = false;
    }
    return found ? blocked : true;
}

int open_ioctl() { return ::open(kIoctlNode, O_RDWR); }

} // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "--check";

    if (cmd == "--check") {
        const int up = count_up_interfaces("ccmni");
        std::printf("bearer   : %s (%d interfaces UP)\n", up == 0 ? "no data" : "ACTIVE", up);
        std::printf("wifi     : %s\n", wifi_blocked() ? "blocked" : "ON");
        std::printf("bluetooth: %s\n", bt_blocked() ? "blocked" : "ON");
        const bool ok = (up == 0) && wifi_blocked() && bt_blocked();
        std::printf("precondition: %s\n", ok ? "OK" : "NOT MET (turn the radios off first)");
        return ok ? 0 : 2;
    }

    if (cmd == "--enter") {
        if ((count_up_interfaces("ccmni") != 0) || !wifi_blocked() || !bt_blocked()) {
            std::fprintf(stderr, "iqmode: precondition not met; not entering\n");
            return 2;
        }
        int fd = open_ioctl();
        if (fd < 0) { std::perror("iqmode: open"); return 3; }
        if (::ioctl(fd, kEnterSdrMode, 0) != 0) { std::perror("iqmode: enter"); ::close(fd); return 4; }
        ::close(fd);
        std::printf("iqmode: SDR mode active (memory released; the phone's radios are silent)\n");
        return 0;
    }

    if (cmd == "--exit") {
        int fd = open_ioctl();
        if (fd < 0) { std::perror("iqmode: open"); return 3; }
        if (::ioctl(fd, kExitSdrMode, 0) != 0) { std::perror("iqmode: exit"); ::close(fd); return 4; }
        ::close(fd);
        std::printf("iqmode: leaving SDR mode; restoring service\n");
        std::system("systemctl restart ofono >/dev/null 2>&1 || true");
        std::printf("iqmode: done; check registration with AT and bring the connection up if the bearer is missing\n");
        return 0;
    }

    std::printf("usage: iqmode [--check|--enter|--exit]\n");
    return 1;
}
