// Tests of the window logic: `--status` parsing, commands and codes.
// They need neither GTK nor the phone: they are built and run on the laptop.
#include <cstdio>
#include <string>
#include <vector>

#include "actions.hpp"
#include "status.hpp"

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

const char* kActiveStatus =
    "sdr-load 1.2 --status\n"
    "      state file: /var/lib/sdr-load/state.json\n"
    "      state     : sdr-active\n"
    "      pending   : -\n"
    "      updated   : 2026-11-05T20:10:00Z\n"
    "      image     : /home/furios/standard-a.img\n"
    "      sha256    : f2c84f9f654e9d73d82e5bf4599ee39b6808da02b1d6e6cd23aa8f3a681022e3\n"
    "      bytes     : 63018064\n"
    "      partition : /dev/disk/by-partlabel/modem\n"
    "      radios    : wifi=yes bt=yes (blocked by the tool)\n"
    "      profile   : standard-a\n"
    "      activation: loops=armed, rails=on\n"
    "      last error: -\n"
    "      observed  : SDR active with dump\n"
    "      SDR mode  :\n"
    "        image   : /dev/disk/by-partlabel/modem (60.1 MiB usable)\n"
    "        sha256  : f2c84f9f654e9d73d82e5bf4599ee39b6808da02b1d6e6cd23aa8f3a681022e3\n"
    "        profile : standard-a (image table), image verified by sha256\n"
    "        PLL     : channel 4, band 77, 4070010 kHz\n"
    "        rails   : VRF18=0x1 VFE28=0x1 VRFCK=0x1\n"
    "        keepalive: ARMED (PASS=12 PER=100 TCK=3, writes 12/12/12)\n"
    "        table   : ARMED (PASS=9 NEG=0 BAD=0 CDFFE=256,656 BSIE=256)\n"
    "        ring    : seq 100 -> 120, copies 5 -> 7, blocks 9 (2000 ms) -> LATE\n"
    "      next      : to go back: sdr-load --restore\n";

const char* kEmptyStatus =
    "sdr-load 1.2 --status\n"
    "      state file: /var/lib/sdr-load/state.json\n"
    "      state     : none (no state file at /var/lib/sdr-load/state.json)\n"
    "      rfkill: phy0       wlan     soft=no hard=no\n"
    "      wlan* interfaces up : wlan0\n"
    "      window: not available (No such file or directory)\n"
    "      observed  : undetermined\n"
    "      SDR mode  :\n"
    "        image   : cannot infer the partition from the flash script; use --partition\n"
    "        profile : UNKNOWN (cannot identify the partition image); "
    "pass --profile <name|file>)\n"
    "        markers : not available (No such file or directory)\n"
    "      next      : no state file yet: if the modem does not come back, use --restore\n";

// The canonical file exists but this user cannot read it: the tool says so and reports
// nothing from it. It must never be shown as "factory image" or "no saved state".
const char* kUnreadableStatus =
    "sdr-load 1.2 --status\n"
    "      state file: /var/lib/sdr-load/state.json\n"
    "      state     : unreadable (Permission denied)\n"
    "      rfkill: phy0       wlan     soft=no hard=no\n"
    "      window: not available (No such file or directory)\n"
    "      observed  : undetermined\n"
    "      SDR mode  :\n"
    "        image   : cannot read /dev/disk/by-partlabel/modem: Permission denied\n"
    "        profile : UNKNOWN (cannot identify the partition image); "
    "pass --profile <name|file>)\n"
    "        markers : not available (No such file or directory)\n"
    "      next      : the state file cannot be read: run it with privileges "
    "(sudo sdr-load --status)\n";

const char* kServiceStatus =
    "sdr-load 1.2 --status\n"
    "      state     : ok\n"
    "      pending   : -\n"
    "      observed  : modem in service\n"
    "      SDR mode  :\n"
    "        image   : /dev/disk/by-partlabel/modem (60.1 MiB usable)\n"
    "        profile : standard-a (image table), image verified by sha256\n"
    "        ring    : seq 7 -> 7, copies 0 -> 0, blocks 0 (2000 ms) -> QUIET\n"
    "      next      : SDR mode with the loops stopped: sdr-load --activate\n";

void test_active_status() {
    const fmiq::StatusInfo s = fmiq::parse_status(kActiveStatus);
    CHECK(s.parsed);
    CHECK(s.state == "sdr-active");
    CHECK(s.updated == "2026-11-05T20:10:00Z");
    CHECK(s.radios == "wifi=yes bt=yes (blocked by the tool)");
    CHECK(s.observable);
    CHECK(s.sdr_active);
    CHECK(s.profile_known);
    CHECK(s.profile == "standard-a");
    CHECK(s.profile_extra == "image verified by sha256");
    CHECK(has(s.image, "/dev/disk/by-partlabel/modem"));
    CHECK(has(s.image, "60.1 MiB usable"));
    CHECK(s.sha256.rfind("f2c84f9f", 0) == 0);
    CHECK(has(s.pll, "channel 4"));
    CHECK(has(s.pll, "band 77"));
    CHECK(s.loops == "armed");
    CHECK(s.rails == "on");
    CHECK(s.ring == "late");
    CHECK(s.last_error == "-");
    CHECK(s.hint == "to go back: sdr-load --restore");
    CHECK(fmiq::mode_text(s) == "SDR mode active, with dump");
}

void test_empty_status() {
    const fmiq::StatusInfo s = fmiq::parse_status(kEmptyStatus);
    CHECK(s.parsed);
    CHECK(has(s.state, "none"));
    CHECK(!s.observable);
    CHECK(!s.sdr_active);
    CHECK(!s.profile_known);
    CHECK(s.profile == "UNKNOWN");
    CHECK(has(s.profile_extra, "cannot identify"));
    CHECK(s.ring.empty());
    CHECK(s.hint == "no state file yet: if the modem does not come back, use --restore");
    CHECK(has(fmiq::mode_text(s), "Undetermined"));
}

void test_unreadable_status() {
    const fmiq::StatusInfo s = fmiq::parse_status(kUnreadableStatus);
    CHECK(s.parsed);
    CHECK(has(s.state, "unreadable"));
    CHECK(has(s.state, "Permission denied"));
    // It is not "no saved state": the window must not turn this into a factory image.
    CHECK(s.state.rfind("none", 0) != 0);
    CHECK(!s.observable);
    CHECK(!s.sdr_active);
    CHECK(s.hint == "the state file cannot be read: run it with privileges (sudo sdr-load --status)");
    CHECK(has(fmiq::mode_text(s), "Undetermined"));
}

void test_service_status() {
    const fmiq::StatusInfo s = fmiq::parse_status(kServiceStatus);
    CHECK(s.parsed);
    CHECK(s.state == "ok");
    CHECK(s.observable);
    CHECK(!s.sdr_active);
    CHECK(s.profile_known);
    CHECK(s.ring == "quiet");
    CHECK(fmiq::mode_text(s) == "In service (standard mode)");
}

void test_quiet_mode_text() {
    std::string text(kServiceStatus);
    text.replace(text.find("modem in service"), std::string("modem in service").size(),
                 "SDR active without dump");
    const fmiq::StatusInfo s = fmiq::parse_status(text);
    CHECK(s.sdr_active);
    CHECK(fmiq::mode_text(s) == "SDR mode active, no dump");
}

void test_argv() {
    const auto status = fmiq::argv_for(fmiq::Action::Status, "/opt/sdr-load");
    CHECK(status.size() == 2 && status[0] == "/opt/sdr-load" && status[1] == "--status");

    const auto flash =
        fmiq::argv_for(fmiq::Action::FlashImage, "/opt/sdr-load", "/tmp/a b.img");
    CHECK(flash.size() == 5);
    CHECK(flash[0] == "/opt/sdr-load");
    CHECK(flash[1] == "--image");
    CHECK(flash[2] == "/tmp/a b.img");
    CHECK(flash[3] == "--yes");
    CHECK(flash[4] == "--no-reboot");

    const auto enter = fmiq::argv_for(fmiq::Action::EnterMode, "sdr-load");
    CHECK(enter.size() == 3 && enter[1] == "--no-flash" && enter[2] == "--yes");

    const auto activate = fmiq::argv_for(fmiq::Action::Activate, "sdr-load");
    CHECK(activate.size() == 3 && activate[1] == "--activate" && activate[2] == "--yes");

    const auto deactivate = fmiq::argv_for(fmiq::Action::Deactivate, "sdr-load");
    CHECK(deactivate.size() == 3 && deactivate[1] == "--deactivate");

    const auto restore = fmiq::argv_for(fmiq::Action::Restore, "sdr-load");
    CHECK(restore.size() == 4 && restore[1] == "--restore" && restore[3] == "--no-reboot");
}

void test_command_text() {
    CHECK(fmiq::command_text({"/usr/local/bin/sdr-load", "--image", "/tmp/a b.img"}) ==
          "/usr/local/bin/sdr-load --image '/tmp/a b.img'");
    CHECK(fmiq::command_text({"sdr-load", "--profile", "a'b"}) ==
          "sdr-load --profile 'a'\\''b'");
    CHECK(fmiq::command_text({"sdr-load", "--status"}) == "sdr-load --status");
}

void test_exit_codes() {
    const auto ok = fmiq::describe_exit(fmiq::Action::Activate, 0);
    CHECK(ok.ok);
    CHECK(ok.headline == "Reception active");

    const auto no_profile = fmiq::describe_exit(fmiq::Action::Activate, 13);
    CHECK(!no_profile.ok);
    CHECK(has(no_profile.headline, "profile"));
    CHECK(has(no_profile.detail, "profile table"));

    const auto failed = fmiq::describe_exit(fmiq::Action::Activate, 14);
    CHECK(!failed.ok);
    CHECK(failed.headline == "Activation failed");
    CHECK(has(failed.detail, "loops"));

    const auto flashed = fmiq::describe_exit(fmiq::Action::FlashImage, 6);
    CHECK(flashed.ok);
    CHECK(flashed.reboot);
    CHECK(has(flashed.headline, "Image written"));

    const auto pending = fmiq::describe_exit(fmiq::Action::EnterMode, 6);
    CHECK(!pending.ok);
    CHECK(pending.reboot);

    const auto recovery = fmiq::describe_exit(fmiq::Action::Restore, 10);
    CHECK(!recovery.ok);
    CHECK(recovery.reboot);

    const auto sig = fmiq::describe_exit(fmiq::Action::Activate, 137);
    CHECK(!sig.ok);
    CHECK(has(sig.headline, "signal"));

    for (int code = 0; code <= 14; ++code) {
        const auto e = fmiq::describe_exit(fmiq::Action::Deactivate, code);
        CHECK(!e.headline.empty());
    }
    CHECK(!fmiq::describe_exit(fmiq::Action::Status, 99).headline.empty());
}

void test_specs() {
    CHECK(!fmiq::spec_for(fmiq::Action::Status).needs_root);
    CHECK(fmiq::spec_for(fmiq::Action::FlashImage).needs_root);
    CHECK(fmiq::spec_for(fmiq::Action::EnterMode).needs_root);
    CHECK(fmiq::spec_for(fmiq::Action::Activate).needs_root);
    CHECK(fmiq::spec_for(fmiq::Action::Deactivate).needs_root);
    CHECK(fmiq::spec_for(fmiq::Action::Restore).needs_root);

    CHECK(fmiq::spec_for(fmiq::Action::FlashImage).writes_partition);
    CHECK(fmiq::spec_for(fmiq::Action::Restore).writes_partition);
    CHECK(!fmiq::spec_for(fmiq::Action::EnterMode).writes_partition);
    CHECK(!fmiq::spec_for(fmiq::Action::Activate).writes_partition);
    CHECK(!fmiq::spec_for(fmiq::Action::Deactivate).writes_partition);

    CHECK(std::string(fmiq::action_id(fmiq::Action::FlashImage)) == "flash");
    CHECK(std::string(fmiq::action_id(fmiq::Action::Restore)) == "restore");
}

}  // namespace

int main() {
    test_active_status();
    test_empty_status();
    test_unreadable_status();
    test_service_status();
    test_quiet_mode_text();
    test_argv();
    test_command_text();
    test_exit_codes();
    test_specs();

    if (g_failed) {
        std::fprintf(stderr, "%d checks, with failures\n", g_checks);
        return 1;
    }
    std::printf("%d checks OK\n", g_checks);
    return 0;
}
