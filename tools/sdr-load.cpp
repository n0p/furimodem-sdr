// sdr-load: loads and reverts the device's SDR mode.
//
// Work order (every step is logged and verified, never assumed):
//   1. disable all radios (WiFi and Bluetooth through rfkill, the modem through the
//      enter-SDR-mode ioctl) and check that no data interface is left up;
//   2. load the mode (firmware image already flashed, or flash + sha256 verification);
//   3. verify that the mode is active: link rings quiet and dump-engine mark present
//      in the working window;
//   4. activate SDR mode with --activate (wait for the modem, arm the dump, seed the
//      PLL, power the rails and start both loops, verifying every step) and stop it
//      with --deactivate (loops only: the rails are NOT powered down in software);
//   5. revert with --restore: clean exit, radios as they were and, if the modem does
//      not come back, recovery image and reboot as a last resort.
//
// Activation carries no hardcoded mode numbers: they come from a per-image table
// (sha256 -> profile) or from a profile file (--profile). If the image is not in the
// table, --activate refuses instead of asking for modes blindly.
//
// The state is persisted in /var/lib/sdr-load/state.json BEFORE every destructive
// step, so that after a reboot the program itself knows where it stopped (--status,
// --verify-boot, --resume). That file is the only state: an unprivileged --status
// reads the same one (0644) and, when it cannot, says so instead of inventing a state.
//
// The state file is a record of what the tool did, not the truth about the phone: the
// partition is. Anything else may have written it (a stock flash, a recovery, a failed
// revert), so --status, --activate and --resume hash the partition and compare it with
// both the state and the profile before believing a word of the state. When they
// disagree, they name the image that is really there and the one the state recorded,
// and the next step is to write the image again, not to repeat the activation.
//
// Exit code 12 is not a dead end either: the modem publishes in bursts (measured:
// 60-200 captures over 10-45 s, then silence), so the dump-engine mark is looked for
// during a window (--mark-timeout-ms, 45 s by default) before it is called missing,
// and the message says whether the image in the partition is the right one.
//
// Exit codes (one per failure, see README):
//   0 OK                      7 SDR mode ioctl failed
//   1 bad usage               8 mode verification failed (rings moving)
//   2 precondition            9 incomplete revert (manual intervention needed)
//   3 radios not blocked     10 recovery image flashed; reboot needed
//   4 flash failed           11 reboot in progress (last resort)
//   5 sha256 mismatch        12 mode active but no dump programmed (mark missing)
//   6 reboot needed          13 image without a mode profile (--activate refuses)
//                            14 activation failed (the loop has been disarmed)
//
// No dependencies: POSIX + libstdc++. It emits no RF: it only blocks radios, writes
// the firmware partition and reads the working window.

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Device constants. This file is the only one that knows them.
// ---------------------------------------------------------------------------

constexpr const char* kIoctlNode = "/dev/ccci_ioctl0";
constexpr unsigned long kIoctlEnterSdr = 0x430E;
constexpr unsigned long kIoctlExitSdr = 0x430F;

constexpr const char* kWindowNode = "/dev/ccci_raw_mdm";
constexpr unsigned long kIoctlWindowBase = 0x80044330;
constexpr unsigned long kIoctlWindowLen = 0x80044331;
// VA base of the modem mirror. Ioctl 0x80044330 returns the HOST address of the mmap
// (it changes between boots: 0x48b00000, 0x8ab00000...), not the modem's VA base; the
// addresses published by the firmware (markers, ring) are measured against this one.
constexpr uint32_t kModemVaBase = 0x48B00000;

constexpr const char* kDefaultFlashScript = "/home/furios/flash_modem.sh";
constexpr const char* kInstalledFlashScript = "/usr/share/furimodem-sdr/flash_modem.sh";
constexpr const char* kDefaultRecoverImage = "/home/furios/modem-E6.img";
// The E6 image the FLX1 shipped: identifying it by sha256 does not depend on the user
// keeping a copy of the file on the phone.
constexpr const char* kStockSha256 =
    "22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896";
constexpr uint64_t kStockImageSize = 63018064;
constexpr const char* kDefaultStatePath = "/var/lib/sdr-load/state.json";

// Offsets inside the link working window (base published by the node).
constexpr uint32_t kWinData = 0x00020000;      // dump engine words
constexpr uint32_t kWinDataLen = 0x1FFC0;      // 128 KiB - 64 B
constexpr uint32_t kWinMark = 0x00040000;      // mark: the engine was left programmed
constexpr uint32_t kWinMark2 = 0x01DF0000;     // second marker (outside the dump)
constexpr uint32_t kMarkMagic = 0x4E524451;    // "NRDQ"

// Ring areas that the modem stack writes in normal mode. In SDR mode they are left
// alone: they are sampled twice and zero differing words are required.
struct RingArea {
    uint32_t off;
    uint32_t len;
    const char* name;
};
constexpr RingArea kRings[] = {
    {0x01800000, 0x00040000, "A"},
    {0x01880000, 0x00060000, "B"},
    {0x01C80000, 0x00100000, "C"},
};

constexpr const char* kToolVersion = "sdr-load 1.2";

// ---------------------------------------------------------------------------
// Modem marker window. The VA published by the firmware is turned into an offset
// inside the window by subtracting its base (the absolute VA is not used: the base
// comes from the ioctl). All of this is measured on the standard-a image (sequence
// measured on the phone).
// ---------------------------------------------------------------------------

constexpr uint32_t kMarkVa = 0x4A8C3600;    // marker block (modem VA)
constexpr uint32_t kMarkSpan = 0x700;       // up to CFG_PER (0x6F0)

// Modem command: the AP writes MGO/MODE and the firmware answers MDON/MGO.
constexpr uint32_t kMarkMgo = 0x600;
constexpr uint32_t kMarkMode = 0x604;
constexpr uint32_t kMarkMdon = 0x608;
constexpr uint32_t kMarkRes = 0x60C;
constexpr uint32_t kMarkStep = 0x610;
constexpr uint32_t kMarkTq = 0x614;

// Heartbeat of the IQ ring and of the copy into the ring.
constexpr uint32_t kMarkSeq = 0x90;         // last seq published
constexpr uint32_t kMarkBlks = 0x94;        // blocks published (total)
constexpr uint32_t kMarkCps = 0x98;         // dumps copied into the ring
constexpr uint32_t kMarkCpsUcnt = 0x9C;     // ucnt of the last copy
// Passive images (modem that copies the scanner capture): their counters live in
// another block and the dump page carries band/frequency at +12.
constexpr uint32_t kMarkPPush = 0x08;       // captures published
constexpr uint32_t kMarkPCaps = 0x0C;       // copies into the ring
constexpr uint32_t kMarkPage3 = 0x300 + 12; // page+12: band (low 16) | kHz (high 16)

// Read table (mode 5) and write-with-readback table (mode 6).
constexpr uint32_t kMarkHwn = 0x700;
constexpr uint32_t kMarkHwa = 0x740;
constexpr uint32_t kMarkHwv = 0x7C0;
constexpr uint32_t kMarkHwr = 0x840;
constexpr uint32_t kPeekMax = 32;          // entries that fit in the table

// Copy of pll_channel_data left by the seed mode (16 words).
constexpr uint32_t kMarkPll = 0x1B0;

// Markers for the end of the cave and for the one-rail-per-mode sequence.
constexpr uint32_t kMarkXStg = 0x948;       // 14900 = the cave return finished
constexpr uint32_t kMarkXFlags = 0x9E8;
constexpr uint32_t kMarkX2Tag = 0xA00;
constexpr uint32_t kMarkX2Mode = 0xA04;
constexpr uint32_t kMarkX2Stg = 0xA08;
constexpr uint32_t kMarkX2Addr = 0xA0C;
constexpr uint32_t kMarkX2V0 = 0xA10;
constexpr uint32_t kMarkX2V1 = 0xA14;
constexpr uint32_t kMarkX2Anom = 0xA18;
constexpr uint32_t kMarkX2Cps0 = 0xA1C;

// Markers of the rail loop (WIT+0x680).
constexpr uint32_t kMarkX3Tag = 0x680;
constexpr uint32_t kMarkX3Mode = 0x684;
constexpr uint32_t kMarkX3Stg = 0x688;
constexpr uint32_t kMarkX3Pass = 0x68C;
constexpr uint32_t kMarkX3W18 = 0x690;
constexpr uint32_t kMarkX3W28 = 0x694;
constexpr uint32_t kMarkX3Wck = 0x698;
constexpr uint32_t kMarkX3V18 = 0x69C;
constexpr uint32_t kMarkX3V28 = 0x6A0;
constexpr uint32_t kMarkX3Vck = 0x6A4;
constexpr uint32_t kMarkX3Anom = 0x6A8;
constexpr uint32_t kMarkX3Armed = 0x6AC;
constexpr uint32_t kMarkX3Per = 0x6B0;
constexpr uint32_t kMarkX3Tck = 0x6B4;
constexpr uint32_t kMarkX3Tn = 0x6B8;
constexpr uint32_t kMarkX3Stop = 0x6C8;
constexpr uint32_t kMarkX3On = 0x6D4;
constexpr uint32_t kMarkCfgPer = 0x6F0;     // keepalive period (written by the AP)

// Markers of the timing-table loop (WIT+0x3E0).
constexpr uint32_t kMarkX4Tag = 0x3E0;
constexpr uint32_t kMarkX4Mode = 0x3E4;
constexpr uint32_t kMarkX4Stg = 0x3E8;
constexpr uint32_t kMarkX4Pass = 0x3EC;
constexpr uint32_t kMarkX4V0 = 0x404;       // last timing table A[0] read
constexpr uint32_t kMarkX4V1 = 0x408;       // last timing table A[1] read
constexpr uint32_t kMarkX4V2 = 0x40C;       // last timing table B[0] read
constexpr uint32_t kMarkX4V3 = 0x410;
constexpr uint32_t kMarkX4V4 = 0x414;
constexpr uint32_t kMarkX4Bad = 0x418;      // readbacks that do not return what was written
constexpr uint32_t kMarkX4On = 0x420;
constexpr uint32_t kMarkX4Neg = 0x424;      // passes that saw a -1
constexpr uint32_t kMarkX4Stop = 0x42C;
constexpr uint32_t kMarkX4Force = 0x430;    // != 0: forced pass
constexpr uint32_t kMarkX4Armed = 0x434;
constexpr uint32_t kMarkX4Fpass = 0x438;

// Tags and magics of the marker blocks.
constexpr uint32_t kTagR150 = 0x30353152;  // "R150"
constexpr uint32_t kTagR156 = 0x36353152;  // "R156"
constexpr uint32_t kTagR159 = 0x39353152;  // "R159"
constexpr uint32_t kMagicLoopRails = 0x314C504B;  // "KPL1": keepalive armed
constexpr uint32_t kMagicLoopTiming = 0x324C504B; // "KPL2": timing loop armed
constexpr uint32_t kCaveDone = 14900;      // end of the cave return

// Addresses of the receiver timing tables (outside the mirror: they are read through
// mode 5, the modem's read table).
constexpr uint32_t kTimingTableA = 0x25F2E228;
constexpr uint32_t kTimingTableB = 0x25F2E1D4;
constexpr uint32_t kRxOnFlag = 0x25F35DC4 + 416;

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

bool g_quiet = false;
std::string g_profile_used;  // profile resolved by the last action (shows up in --json)

void log_step(int i, int n, const char* fmt, ...) {
    if (g_quiet) return;
    std::printf("[%d/%d] ", i, n);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

void log_note(const char* fmt, ...) {
    if (g_quiet) return;
    std::printf("      ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

void log_warn(const char* fmt, ...) {
    std::printf("  WARNING: ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

void log_error(const char* fmt, ...) {
    std::fprintf(stderr, "sdr-load: ERROR: ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// External command execution (no shell: no path interpolation)
// ---------------------------------------------------------------------------

int run_inherit(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::execvp(args[0], args.data());
        std::perror("sdr-load: exec");
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int run_capture(const std::vector<std::string>& argv, std::string* out) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    int pipefd[2];
    if (::pipe(pipefd) != 0) return -1;
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string buf;
    char tmp[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], tmp, sizeof(tmp))) > 0) buf.append(tmp, static_cast<size_t>(n));
    ::close(pipefd[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (out) *out = buf;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

bool command_exists(const char* name) {
    std::string out;
    return run_capture({"sh", "-c", std::string("command -v ") + name}, &out) == 0 && !out.empty();
}

// Launches a command and does not wait: the child gets its own session. Used for the
// operator scan, which does not finish while the sweep is stuck on one frequency.
bool run_detached(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        int fd = ::open("/dev/null", O_RDWR);
        if (fd >= 0) {
            ::dup2(fd, STDIN_FILENO);
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) ::close(fd);
        }
        // Do not inherit the modem window: it would leave /dev/ccci_raw_mdm busy until
        // the scan finishes (600 s) and the next reader could not open it.
        for (int i = 3; i < 64; ++i) ::close(i);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    return true;
}

// Sends an AT command through the ofono interface. Returns true only if the reply contains OK.
bool at_send(const std::string& cmd, std::string* out, int timeout_s) {
    if (!command_exists("busctl")) {
        if (out) *out = "busctl is not available";
        return false;
    }
    std::vector<std::string> argv = {"busctl", "--timeout=" + std::to_string(timeout_s),
                                     "call", "org.ofono", "/ril_0",
                                     "org.ofono.FuriLabs.AT", "SendCommand", "s",
                                     "\"" + cmd + "\""};
    std::string buf;
    const int rc = run_capture(argv, &buf);
    if (out) *out = buf;
    return rc == 0 && buf.find("OK") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    struct tm tm {};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool read_file(const std::string& path, std::string* out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    std::string buf;
    char tmp[512];
    ssize_t n;
    while ((n = ::read(fd, tmp, sizeof(tmp))) > 0) buf.append(tmp, static_cast<size_t>(n));
    if (n < 0) {
        const int e = errno;
        ::close(fd);
        errno = e;  // close() may clobber it; the caller wants to know why it failed
        return false;
    }
    ::close(fd);
    if (out) *out = buf;
    return true;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool hex_to_u64(const std::string& in, uint64_t* out) {
    std::string s = trim(in);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<uint64_t>(d);
        if (v > 0xFFFFFFFFull) return false;
    }
    *out = v;
    return true;
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), no dependencies
// ---------------------------------------------------------------------------

class Sha256 {
  public:
    Sha256() { reset(); }

    void reset() {
        h_[0] = 0x6a09e667u;
        h_[1] = 0xbb67ae85u;
        h_[2] = 0x3c6ef372u;
        h_[3] = 0xa54ff53au;
        h_[4] = 0x510e527fu;
        h_[5] = 0x9b05688cu;
        h_[6] = 0x1f83d9abu;
        h_[7] = 0x5be0cd19u;
        len_ = 0;
        fill_ = 0;
    }

    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len_ += n;
        while (n > 0) {
            size_t take = std::min(n, sizeof(buf_) - fill_);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take;
            p += take;
            n -= take;
            if (fill_ == sizeof(buf_)) {
                block(buf_);
                fill_ = 0;
            }
        }
    }

    std::string hex() {
        uint64_t bits = len_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (fill_ != 56) update(&zero, 1);
        uint8_t lenbuf[8];
        for (int i = 7; i >= 0; --i) lenbuf[i] = static_cast<uint8_t>(bits >> (8 * (7 - i)));
        update(lenbuf, 8);

        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t v : h_) {
            for (int i = 3; i >= 0; --i) {
                uint8_t byte = static_cast<uint8_t>(v >> (8 * i));
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 0xf]);
            }
        }
        return out;
    }

  private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) |
                   (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(p[4 * i + 2]) << 8) |
                   static_cast<uint32_t>(p[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t len_;
    size_t fill_;
};

std::string sha256_of_string(const std::string& s) {
    Sha256 h;
    h.update(s.data(), s.size());
    return h.hex();
}

bool sha256_of_file(const std::string& path, uint64_t limit, std::string* hex, uint64_t* size,
                    std::string* err) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (err) *err = std::strerror(errno);
        return false;
    }
    Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t total = 0;
    while (limit == 0 || total < limit) {
        size_t want = buf.size();
        if (limit != 0) want = static_cast<size_t>(std::min<uint64_t>(want, limit - total));
        ssize_t n = ::read(fd, buf.data(), want);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err) *err = std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        h.update(buf.data(), static_cast<size_t>(n));
        total += static_cast<uint64_t>(n);
    }
    ::close(fd);
    if (hex) *hex = h.hex();
    if (size) *size = total;
    return true;
}

// ---------------------------------------------------------------------------
// rfkill
// ---------------------------------------------------------------------------

struct RfkillEntry {
    std::string path;
    std::string name;
    std::string type;
    bool soft = false;
    bool hard = false;
};

std::vector<RfkillEntry> rfkill_scan() {
    std::vector<RfkillEntry> out;
    for (int i = 0; i < 64; ++i) {
        std::string base = "/sys/class/rfkill/rfkill" + std::to_string(i);
        std::string type, name, soft, hard;
        if (!read_file(base + "/type", &type)) continue;
        read_file(base + "/name", &name);
        read_file(base + "/soft", &soft);
        read_file(base + "/hard", &hard);
        RfkillEntry e;
        e.path = base;
        e.type = trim(type);
        e.name = trim(name);
        e.soft = trim(soft) == "1";
        e.hard = trim(hard) == "1";
        out.push_back(e);
    }
    return out;
}

bool rfkill_set_soft(RfkillEntry& e, bool block) {
    int fd = ::open((e.path + "/soft").c_str(), O_WRONLY);
    if (fd < 0) return false;
    const char* v = block ? "1" : "0";
    ssize_t n = ::write(fd, v, 1);
    ::close(fd);
    if (n != 1) return false;
    std::string now;
    if (!read_file(e.path + "/soft", &now)) return false;
    e.soft = trim(now) == "1";
    return e.soft == block;
}

// ---------------------------------------------------------------------------
// Network interfaces
// ---------------------------------------------------------------------------

struct NetIface {
    std::string name;
    bool up = false;
};

std::vector<NetIface> net_scan() {
    std::vector<NetIface> out;
    DIR* d = ::opendir("/sys/class/net");
    if (!d) return out;
    struct dirent* de;
    while ((de = ::readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string flags_s;
        if (!read_file("/sys/class/net/" + name + "/flags", &flags_s)) continue;
        uint64_t flags = 0;
        if (!hex_to_u64(flags_s, &flags)) continue;
        NetIface ni;
        ni.name = name;
        ni.up = (flags & 0x1ull) != 0;  // IFF_UP
        out.push_back(ni);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const NetIface& a, const NetIface& b) {
        return a.name < b.name;
    });
    return out;
}

std::vector<std::string> up_interfaces_with_prefix(const char* prefix, bool* scan_ok = nullptr) {
    std::vector<std::string> out;
    std::vector<NetIface> all = net_scan();
    if (scan_ok) *scan_ok = !all.empty() || ::access("/sys/class/net", F_OK) == 0;
    std::string p(prefix);
    for (const auto& ni : all) {
        if (ni.up && ni.name.compare(0, p.size(), p) == 0) out.push_back(ni.name);
    }
    return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Link working window (node mmap, 32-bit reads only)
// ---------------------------------------------------------------------------

class Window {
  public:
    ~Window() { close(); }

    bool open(std::string* err) { return open_mode(err, false); }
    bool open_rw(std::string* err) { return open_mode(err, true); }

    bool open_mode(std::string* err, bool writable) {
        int fd = ::open(kWindowNode, (writable ? O_RDWR : O_RDONLY) | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) fd = ::open(kWindowNode, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (err) *err = std::strerror(errno);
            return false;
        }
        uint32_t base = 0, len = 0;
        if (::ioctl(fd, kIoctlWindowBase, &base) != 0 || ::ioctl(fd, kIoctlWindowLen, &len) != 0) {
            if (err) *err = std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (len < kWinMark + 4) {
            if (err) *err = "window too small";
            ::close(fd);
            return false;
        }
        void* m = ::mmap(nullptr, len, writable ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED,
                         fd, 0);
        if (m == MAP_FAILED) {
            if (err) *err = std::strerror(errno);
            ::close(fd);
            return false;
        }
        fd_ = fd;
        map_ = m;
        len_ = len;
        base_ = base;
        writable_ = writable;
        return true;
    }

    void close() {
        if (map_) ::munmap(map_, len_);
        if (fd_ >= 0) ::close(fd_);
        map_ = nullptr;
        fd_ = -1;
        writable_ = false;
    }

    bool valid() const { return map_ != nullptr; }
    bool writable() const { return writable_; }
    uint32_t base() const { return base_; }
    size_t length() const { return len_; }

    bool has(uint32_t off, uint32_t len) const {
        return static_cast<size_t>(off) + len <= len_;
    }

    // Modem VA -> offset inside the window. The modem VA base is fixed
    // (kModemVaBase); the one the ioctl returns is the mmap base in the host.
    bool va_to_off(uint32_t va, uint32_t len, uint32_t* off) const {
        if (va < kModemVaBase) return false;
        const uint32_t o = va - kModemVaBase;
        if (static_cast<uint64_t>(o) + len > len_) return false;
        *off = o;
        return true;
    }

    uint32_t rd32(uint32_t off) const {
        const volatile uint32_t* p = static_cast<const volatile uint32_t*>(map_);
        return p[off / 4];
    }

    // 32-bit write. Read/write mode only, and inside the window.
    bool wr32(uint32_t off, uint32_t val) const {
        if (!writable_ || !has(off, 4)) return false;
        volatile uint32_t* p = static_cast<volatile uint32_t*>(map_);
        p[off / 4] = val;
        return true;
    }

    void snapshot(uint32_t off, uint32_t len, std::vector<uint32_t>* out) const {
        const volatile uint32_t* p = static_cast<const volatile uint32_t*>(map_);
        size_t n = len / 4;
        out->resize(n);
        for (size_t i = 0; i < n; ++i) (*out)[i] = p[off / 4 + i];
    }

  private:
    int fd_ = -1;
    void* map_ = nullptr;
    size_t len_ = 0;
    uint32_t base_ = 0;
    bool writable_ = false;
};

struct RingDiff {
    uint64_t changed_words = 0;
    uint64_t bytes_sampled = 0;
    std::vector<std::pair<std::string, uint64_t>> per_ring;
    bool moving() const { return changed_words != 0; }
};

RingDiff diff_snapshots(const std::vector<std::vector<uint32_t>>& a,
                        const std::vector<std::vector<uint32_t>>& b) {
    RingDiff d;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        uint64_t changed = 0;
        size_t n = std::min(a[i].size(), b[i].size());
        for (size_t j = 0; j < n; ++j)
            if (a[i][j] != b[i][j]) ++changed;
        d.changed_words += changed;
        d.per_ring.push_back({std::string(i < 3 ? kRings[i].name : "?"), changed});
        d.bytes_sampled += static_cast<uint64_t>(n) * 4;
    }
    return d;
}

bool sample_rings(const Window& w, std::vector<std::vector<uint32_t>>* out, std::string* err) {
    if (!w.valid()) {
        if (err) *err = "window not available";
        return false;
    }
    out->clear();
    for (const auto& r : kRings) {
        if (static_cast<size_t>(r.off) + r.len > w.length()) {
            if (err) *err = "ring area outside the window";
            return false;
        }
        std::vector<uint32_t> s;
        w.snapshot(r.off, r.len, &s);
        out->push_back(std::move(s));
    }
    return true;
}

RingDiff measure_rings(const Window& w, int gap_ms, int samples, std::string* err) {
    std::vector<std::vector<uint32_t>> prev, cur;
    RingDiff total;
    if (!sample_rings(w, &prev, err)) return total;
    for (int i = 1; i < samples; ++i) {
        ::usleep(static_cast<useconds_t>(gap_ms) * 1000);
        if (!sample_rings(w, &cur, err)) return total;
        RingDiff d = diff_snapshots(prev, cur);
        total.changed_words += d.changed_words;
        total.bytes_sampled = d.bytes_sampled;
        if (total.per_ring.empty()) total.per_ring = d.per_ring;
        else
            for (size_t k = 0; k < d.per_ring.size() && k < total.per_ring.size(); ++k)
                total.per_ring[k].second += d.per_ring[k].second;
        prev.swap(cur);
    }
    return total;
}

uint64_t count_nonzero(const Window& w, uint32_t off, uint32_t len) {
    uint64_t nz = 0;
    for (uint32_t i = 0; i + 4 <= len; i += 4)
        if (w.rd32(off + i) != 0) ++nz;
    return nz;
}

// ---------------------------------------------------------------------------
// Image profiles: mode number -> meaning
//
// The same mode number does different things in each image: the patched firmware
// dispatches it, not this program. That is why the activation sequence carries no
// hardcoded numbers: they come from a profile, chosen by the image sha256 (table
// below) or by --profile <file>. If the image is not in the table, --activate
// refuses: modes are never requested blindly.
// ---------------------------------------------------------------------------

struct ModeMeaning {
    int mode;
    std::string what;
};

// standard-a = ring + PLL + modes 1..12 in the modem, plus the cave with
// 13/14/15 (PMIC), 27..29 (rails) and 30/31/32 (loops).
const ModeMeaning kModesStandardA[] = {
    {1, "copy the synthetic cell tables (CDFFE/BSIE/PLL/l1d_rf/win) into the markers"},
    {2, "E1: seed PLL/context + SR1 + SR3 (no RX ON)"},
    {3, "E2: seed PLL/context + SR1 + SR3 + forced pass + RX ON"},
    {4, "context seed only (fatal with the rails on: do not use)"},
    {5, "read modem addresses through the table (peek)"},
    {6, "write addresses and read them back (poke): arms the dump engine"},
    {7, "copy one modem block into the markers"},
    {10, "receive DFE clock (RX_Init)"},
    {11, "force the RXDFESYS AO clock"},
    {12, "release the AO clock force"},
    {13, "read CON0 of VRF18/VFE28/VRFCK/VRF12 (read only)"},
    {14, "CON0 |= 1 on the three RF rails, with a guard"},
    {15, "probe 0xA82B011C (power ready)"},
    {27, "power VRF18: CON0 |= 1 (never 0)"},
    {28, "power VFE28: CON0 |= 1 (never 0)"},
    {29, "power VRFCK: CON0 |= 1 (never 0)"},
    {30, "arm the rail keepalive, set the period and run one pass"},
    {31, "stop both loops (does not touch the PMIC: writes no rail)"},
    {32, "arm the timing table (CDFFE/BSIE) and run one pass"},
};

// standard-b = standard-a + the descriptor window.
const ModeMeaning kModesStandardBExtra[] = {
    {33, "check the window descriptor without calling RX_Win_On"},
    {34, "build the descriptor and call RX_Win_On"},
};

struct ImageProfile {
    std::string name;        // short profile name
    std::string image_hint;  // file name it was built from
    std::string sha256;      // exact sha256 of the image (63,018,064 B)
    std::string note;        // what this image brings
    // Passive image: the modem copies the scanner capture on its own and there are
    // no modes to ask for. It is neither activated nor deactivated: only loaded
    // and read.
    bool passive = false;
    // Sweep image with fixed tuning: it dispatches no mode numbers either, but it is
    // activated. The capture is produced by the firmware scanner and the cave fixes
    // band and frequency before the RF base reads them; activating means starting
    // the scan, requiring the ring to advance and checking the page frequency.
    bool scan = false;
    uint32_t fixed_khz = 0;  // frequency fixed by the cave (0 = not checked)
    int fixed_band = 0;
    int cave_min = 13;       // first mode dispatched by the cave
    int cave_max = 0;        // last mode the modem accepts
    int pll_seed = -1;       // mode that seeds the PLL
    int rail_v18 = -1, rail_v28 = -1, rail_vck = -1;
    int keepalive = -1;      // mode that arms the rail keepalive
    int loop_stop = -1;      // mode that stops both loops
    int timing = -1;         // mode that arms the timing table
    int peek_mode = -1;      // mode that reads addresses (modem table)
    int poke_mode = -1;      // mode that writes addresses and reads them back
    uint32_t arm_addr = 0;   // dump engine arming: address...
    uint32_t arm_value = 0;  // ...and value (0 = the profile does not declare it)
    int pll_channel = 0, pll_band = 0;
    uint32_t pll_khz = 0;
    uint32_t rail_bit = 1;   // CON0 bit that powers the rail
    uint32_t loop_period = 0;  // CFG_PER (0 = not written)
    uint32_t cdf0 = 0, cdf1 = 0, bsi = 0;  // expected timing table
    const ModeMeaning* modes = nullptr;
    size_t n_modes = 0;
    const ModeMeaning* extra = nullptr;
    size_t n_extra = 0;
};

std::vector<ImageProfile> build_known_profiles() {
    std::vector<ImageProfile> v;

    ImageProfile standard_a;
    standard_a.name = "standard-a";
    standard_a.image_hint = "modem-sdr-standard-a.img";
    standard_a.sha256 = "f2c84f9f654e9d73d82e5bf4599ee39b6808da02b1d6e6cd23aa8f3a681022e3";
    standard_a.note = "IQ ring + PLL + rails + loops 30/31/32 (measured)";
    standard_a.cave_min = 13;
    standard_a.cave_max = 32;
    standard_a.pll_seed = 3;
    standard_a.rail_v18 = 27;
    standard_a.rail_v28 = 28;
    standard_a.rail_vck = 29;
    standard_a.keepalive = 30;
    standard_a.loop_stop = 31;
    standard_a.timing = 32;
    standard_a.peek_mode = 5;
    standard_a.poke_mode = 6;
    standard_a.arm_addr = 0x255C6B58;
    standard_a.arm_value = 0x00000100;
    standard_a.pll_channel = 4;
    standard_a.pll_band = 77;
    standard_a.pll_khz = 4070010;
    standard_a.rail_bit = 1;
    standard_a.loop_period = 100;
    standard_a.cdf0 = 256;
    standard_a.cdf1 = 656;
    standard_a.bsi = 256;
    standard_a.modes = kModesStandardA;
    standard_a.n_modes = sizeof(kModesStandardA) / sizeof(kModesStandardA[0]);
    v.push_back(standard_a);

    ImageProfile standard_b;
    standard_b.name = "standard-b";
    standard_b.image_hint = "modem-sdr-standard-b.img";
    standard_b.sha256 = "93ad92915f7724c4814159a162c40be74f07b0d251c605f9bd3d987f6cbc6352";
    standard_b.note = "standard-a + descriptor window (modes 33/34)";
    standard_b.cave_min = 13;
    standard_b.cave_max = 34;
    standard_b.pll_seed = 3;
    standard_b.rail_v18 = 27;
    standard_b.rail_v28 = 28;
    standard_b.rail_vck = 29;
    standard_b.keepalive = 30;
    standard_b.loop_stop = 31;
    standard_b.timing = 32;
    standard_b.peek_mode = 5;
    standard_b.poke_mode = 6;
    standard_b.arm_addr = 0x255C6B58;
    standard_b.arm_value = 0x00000100;
    standard_b.pll_channel = 4;
    standard_b.pll_band = 77;
    standard_b.pll_khz = 4070010;
    standard_b.rail_bit = 1;
    standard_b.loop_period = 100;
    standard_b.cdf0 = 256;
    standard_b.cdf1 = 656;
    standard_b.bsi = 256;
    standard_b.modes = kModesStandardA;
    standard_b.n_modes = sizeof(kModesStandardA) / sizeof(kModesStandardA[0]);
    standard_b.extra = kModesStandardBExtra;
    standard_b.n_extra = sizeof(kModesStandardBExtra) / sizeof(kModesStandardBExtra[0]);
    v.push_back(standard_b);

    // sdr-8bit = stock image + a modem that copies the capture the scanner already
    // makes + an 8-bit cave with origin 3 + tag + tuning at the RF base. It dispatches
    // no mode numbers: it is activated by starting the firmware scan, and the check is
    // that the ring advances and the page stays at 2,115.0 MHz (band 1).
    ImageProfile sdr_8bit;
    sdr_8bit.name = "sdr-8bit";
    sdr_8bit.image_hint = "modem-sdr-8bit.img";
    sdr_8bit.sha256 = "5a625327a06fcacf12f611419437bc85aa54c339911a8d3427262fea0643b4d3";
    sdr_8bit.note = "8 bits per component, origin 3, fixed tuning at 2,115.0 MHz (band 1)";
    sdr_8bit.scan = true;
    sdr_8bit.fixed_khz = 21150;
    sdr_8bit.fixed_band = 1;
    v.push_back(sdr_8bit);

    return v;
}

const std::vector<ImageProfile>& known_profiles() {
    static const std::vector<ImageProfile> v = build_known_profiles();
    return v;
}

std::string known_profile_names() {
    std::string s;
    for (const auto& p : known_profiles()) {
        if (!s.empty()) s += ", ";
        s += p.name;
    }
    return s;
}

// Resolved profile (from the table or from a file): what the sequence consumes.
struct ModeMap {
    bool ok = false;
    std::string name;
    std::string source;      // "image table" or the file path
    std::string sha256;      // required sha256 ("" = not required)
    std::string image_hint;
    std::string note;
    bool passive = false;    // self-capturing image: no modes to ask for
    bool scan = false;       // sweep image with fixed tuning: activated by starting the scan
    uint32_t fixed_khz = 0;  // frequency fixed by the cave (0 = not checked)
    int fixed_band = 0;
    int cave_min = 13;
    int cave_max = 0;
    int pll_seed = -1, rail_v18 = -1, rail_v28 = -1, rail_vck = -1;
    int keepalive = -1, loop_stop = -1, timing = -1;
    int peek_mode = -1, poke_mode = -1;
    uint32_t arm_addr = 0, arm_value = 0;
    int pll_channel = 0, pll_band = 0;
    uint32_t pll_khz = 0;
    uint32_t rail_bit = 1;
    uint32_t loop_period = 0;
    uint32_t cdf0 = 0, cdf1 = 0, bsi = 0;
    std::vector<ModeMeaning> modes;

    std::string meaning(int mode) const {
        for (const auto& m : modes)
            if (m.mode == mode) return m.what;
        return "";
    }

    bool describe(int mode, std::string* out) const {
        std::string w = meaning(mode);
        if (w.empty()) {
            if (out) *out = "mode " + std::to_string(mode) + " (no description in the profile)";
            return false;
        }
        if (out) *out = "mode " + std::to_string(mode) + ": " + w;
        return true;
    }

    // The modes the sequence really needs. Without them there is no activation.
    bool complete(std::string* why) const {
        // A passive or sweep image asks for no modes: its modem copies the scanner
        // capture without the AP writing a single mode.
        if (passive || scan) return true;
        struct Req {
            int v;
            const char* key;
        };
        const Req req[] = {{pll_seed, "pll_seed"},   {rail_v18, "rail_v18"},
                           {rail_v28, "rail_v28"},   {rail_vck, "rail_vck"},
                           {keepalive, "keepalive"}, {loop_stop, "loop_stop"},
                           {timing, "timing"},       {peek_mode, "peek_mode"},
                           {poke_mode, "poke_mode"}};
        for (const auto& r : req) {
            if (r.v <= 0) {
                if (why) *why = std::string("the profile does not declare ") + r.key;
                return false;
            }
        }
        if (pll_channel <= 0 || pll_band <= 0 || pll_khz == 0) {
            if (why) *why = "the profile does not declare the expected PLL (pll_channel/pll_band/pll_khz)";
            return false;
        }
        if (cave_max != 0) {
            for (int m : {pll_seed, rail_v18, rail_v28, rail_vck, keepalive, loop_stop, timing}) {
                if (m > cave_max) {
                    if (why)
                        *why = "the profile asks for mode " + std::to_string(m) +
                               ", above the image ceiling (" + std::to_string(cave_max) + ")";
                    return false;
                }
            }
        }
        return true;
    }
};

ModeMap map_from_profile(const ImageProfile& p) {
    ModeMap m;
    m.ok = true;
    m.name = p.name;
    m.source = "image table";
    m.sha256 = p.sha256;
    m.image_hint = p.image_hint;
    m.note = p.note;
    m.passive = p.passive;
    m.scan = p.scan;
    m.fixed_khz = p.fixed_khz;
    m.fixed_band = p.fixed_band;
    m.cave_min = p.cave_min;
    m.cave_max = p.cave_max;
    m.pll_seed = p.pll_seed;
    m.rail_v18 = p.rail_v18;
    m.rail_v28 = p.rail_v28;
    m.rail_vck = p.rail_vck;
    m.keepalive = p.keepalive;
    m.loop_stop = p.loop_stop;
    m.timing = p.timing;
    m.peek_mode = p.peek_mode;
    m.poke_mode = p.poke_mode;
    m.arm_addr = p.arm_addr;
    m.arm_value = p.arm_value;
    m.pll_channel = p.pll_channel;
    m.pll_band = p.pll_band;
    m.pll_khz = p.pll_khz;
    m.rail_bit = p.rail_bit;
    m.loop_period = p.loop_period;
    m.cdf0 = p.cdf0;
    m.cdf1 = p.cdf1;
    m.bsi = p.bsi;
    for (size_t i = 0; i < p.n_modes; ++i) m.modes.push_back(p.modes[i]);
    for (size_t i = 0; i < p.n_extra; ++i) m.modes.push_back(p.extra[i]);
    return m;
}

const ImageProfile* find_profile_by_sha256(const std::string& sha) {
    for (const auto& p : known_profiles())
        if (p.sha256 == sha) return &p;
    return nullptr;
}

const ImageProfile* find_profile_by_name(const std::string& name) {
    for (const auto& p : known_profiles())
        if (p.name == name) return &p;
    return nullptr;
}

// Line number, for the profile file messages.
size_t count_lines(const std::string& s, size_t upto) {
    size_t n = 1;
    for (size_t i = 0; i < upto && i < s.size(); ++i)
        if (s[i] == '\n') ++n;
    return n;
}

bool parse_u32(const std::string& v, uint32_t* out) {
    std::string s = trim(v);
    if (s.empty() || s[0] == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long x = std::strtoull(s.c_str(), &end, 0);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    *out = static_cast<uint32_t>(x);
    return true;
}

// Profile file: `key = value` lines, a leading `#` to comment and
// `mode = N: what it does` for the table. Strict on purpose: an unknown key is an
// error, not silently ignored (a profile with a typo must not activate the wrong
// modes).
bool parse_profile_text(const std::string& text, const std::string& src, ModeMap* m,
                        std::string* err) {
    *m = ModeMap();
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string raw = text.substr(pos, eol - pos);
        const size_t line_no = count_lines(text, pos);
        const std::string line = trim(raw);
        pos = eol + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            if (eol == text.size()) break;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (err) *err = src + ":" + std::to_string(line_no) + ": missing '='";
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        uint32_t num = 0;
        auto need_num = [&](const char* what) -> bool {
            if (!parse_u32(val, &num)) {
                if (err)
                    *err = src + ":" + std::to_string(line_no) + ": " + what +
                           " is not a number: '" + val + "'";
                return false;
            }
            return true;
        };
        if (key == "name") m->name = val;
        else if (key == "sha256") m->sha256 = val;
        else if (key == "note") m->note = val;
        else if (key == "passive") {
            if (!need_num(key.c_str()) || num > 1) {
                if (err) *err = src + ":" + std::to_string(line_no) + ": passive must be 0 or 1";
                return false;
            }
            m->passive = num != 0;
        }
        else if (key == "scan") {
            if (!need_num(key.c_str()) || num > 1) {
                if (err) *err = src + ":" + std::to_string(line_no) + ": scan must be 0 or 1";
                return false;
            }
            m->scan = num != 0;
        }
        else if (key == "fixed_khz") { if (!need_num(key.c_str())) return false; m->fixed_khz = num; }
        else if (key == "fixed_band") { if (!need_num(key.c_str())) return false; m->fixed_band = static_cast<int>(num); }
        else if (key == "cave_min") { if (!need_num(key.c_str())) return false; m->cave_min = static_cast<int>(num); }
        else if (key == "cave_max") { if (!need_num(key.c_str())) return false; m->cave_max = static_cast<int>(num); }
        else if (key == "pll_seed") { if (!need_num(key.c_str())) return false; m->pll_seed = static_cast<int>(num); }
        else if (key == "rail_v18") { if (!need_num(key.c_str())) return false; m->rail_v18 = static_cast<int>(num); }
        else if (key == "rail_v28") { if (!need_num(key.c_str())) return false; m->rail_v28 = static_cast<int>(num); }
        else if (key == "rail_vck") { if (!need_num(key.c_str())) return false; m->rail_vck = static_cast<int>(num); }
        else if (key == "keepalive") { if (!need_num(key.c_str())) return false; m->keepalive = static_cast<int>(num); }
        else if (key == "loop_stop") { if (!need_num(key.c_str())) return false; m->loop_stop = static_cast<int>(num); }
        else if (key == "timing") { if (!need_num(key.c_str())) return false; m->timing = static_cast<int>(num); }
        else if (key == "peek_mode") { if (!need_num(key.c_str())) return false; m->peek_mode = static_cast<int>(num); }
        else if (key == "poke_mode") { if (!need_num(key.c_str())) return false; m->poke_mode = static_cast<int>(num); }
        else if (key == "arm_addr") { if (!need_num(key.c_str())) return false; m->arm_addr = num; }
        else if (key == "arm_value") { if (!need_num(key.c_str())) return false; m->arm_value = num; }
        else if (key == "pll_channel") { if (!need_num(key.c_str())) return false; m->pll_channel = static_cast<int>(num); }
        else if (key == "pll_band") { if (!need_num(key.c_str())) return false; m->pll_band = static_cast<int>(num); }
        else if (key == "pll_khz") { if (!need_num(key.c_str())) return false; m->pll_khz = num; }
        else if (key == "rail_bit") { if (!need_num(key.c_str())) return false; m->rail_bit = num; }
        else if (key == "loop_period") { if (!need_num(key.c_str())) return false; m->loop_period = num; }
        else if (key == "cdf0") { if (!need_num(key.c_str())) return false; m->cdf0 = num; }
        else if (key == "cdf1") { if (!need_num(key.c_str())) return false; m->cdf1 = num; }
        else if (key == "bsi") { if (!need_num(key.c_str())) return false; m->bsi = num; }
        else if (key == "mode") {
            const size_t colon = val.find(':');
            const std::string num_s = trim(colon == std::string::npos ? val : val.substr(0, colon));
            const std::string what = colon == std::string::npos ? "" : trim(val.substr(colon + 1));
            uint32_t mm = 0;
            if (!parse_u32(num_s, &mm) || mm == 0 || mm > 65535) {
                if (err)
                    *err = src + ":" + std::to_string(line_no) + ": invalid mode: '" + num_s + "'";
                return false;
            }
            ModeMeaning e;
            e.mode = static_cast<int>(mm);
            e.what = what;
            m->modes.push_back(e);
        } else {
            if (err) *err = src + ":" + std::to_string(line_no) + ": unknown key: '" + key + "'";
            return false;
        }
        if (eol == text.size()) break;
    }
    m->ok = true;
    m->source = src;
    if (m->name.empty()) m->name = src;
    if (m->cave_max == 0) {
        int hi = 0;
        for (const auto& e : m->modes) hi = std::max(hi, e.mode);
        m->cave_max = hi;
    }
    if (m->modes.empty() && !m->passive && !m->scan) {
        if (err) *err = src + ": the profile declares no mode ('mode = N: what it does')";
        return false;
    }
    return true;
}

bool load_profile_file(const std::string& path, ModeMap* m, std::string* err) {
    std::string text;
    if (!read_file(path, &text)) {
        if (err) *err = "cannot read profile " + path + ": " + std::strerror(errno);
        return false;
    }
    return parse_profile_text(text, path, m, err);
}

void print_mode_table(const ModeMap& m) {
    log_note("profile %s (%s)%s", m.name.c_str(), m.source.c_str(),
             m.note.empty() ? "" : (": " + m.note).c_str());
    if (!m.sha256.empty()) log_note("  sha256 %s", m.sha256.c_str());
    for (const auto& e : m.modes) log_note("  mode %2d  %s", e.mode, e.what.c_str());
    if (m.passive) {
        log_note("  passive image: the modem copies the scanner capture on its own;"
                 " there are no modes to ask for");
        log_note("  use it with --image (or --no-flash if already loaded) and read the ring;"
                 " --activate and --deactivate refuse");
        return;
    }
    if (m.scan) {
        log_note("  sweep image with fixed tuning: the capture is produced by the firmware"
                 " scanner; there are no modes to ask for");
        if (m.fixed_khz != 0)
            log_note("  the cave fixes band %d and %u (x100 kHz) = %.1f MHz at the RF base",
                     m.fixed_band, m.fixed_khz, m.fixed_khz / 10.0);
        log_note("  --activate starts the scan and requires the ring to advance and the page"
                 " to stay on that frequency; --deactivate cuts it");
        return;
    }
    log_note("sequence: %d PLL, %d/%d/%d rails, %d keepalive, %d stop, %d table, %d/%d read/write",
             m.pll_seed, m.rail_v18, m.rail_v28, m.rail_vck, m.keepalive, m.loop_stop, m.timing,
             m.peek_mode, m.poke_mode);
    if (m.arm_addr != 0)
        log_note("arming  : mode %d, %#010x <- %#010x", m.poke_mode, m.arm_addr, m.arm_value);
}

// ---------------------------------------------------------------------------
// Modem marker access (published VA -> window)
// ---------------------------------------------------------------------------

void memory_barrier() { __sync_synchronize(); }

bool wit_read(const Window& w, uint32_t off, uint32_t* v) {
    uint32_t o = 0;
    if (!w.va_to_off(kMarkVa + off, 4, &o)) return false;
    *v = w.rd32(o);
    return true;
}

bool wit_write(Window& w, uint32_t off, uint32_t v) {
    uint32_t o = 0;
    if (!w.va_to_off(kMarkVa + off, 4, &o)) return false;
    return w.wr32(o, v);
}

bool wit_block(const Window& w, uint32_t off, uint32_t words, std::vector<uint32_t>* out) {
    uint32_t o = 0;
    if (!w.va_to_off(kMarkVa + off, words * 4, &o)) return false;
    w.snapshot(o, words * 4, out);
    return true;
}

int64_t mono_ms() {
    struct timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08x", v);
    return b;
}

bool have_getprop() {
    static const bool yes = command_exists("getprop");
    return yes;
}

std::string md1_status(std::string* err = nullptr) {
    if (!have_getprop()) {
        if (err) *err = "getprop is not in PATH";
        return "";
    }
    std::string out;
    if (run_capture({"getprop", "vendor.mtk.md1.status"}, &out) != 0) {
        if (err) *err = "getprop failed";
        return "";
    }
    const std::string v = trim(out);
    if (v.empty() && err) *err = "the property is empty";
    return v;
}

// The modem takes >20 s to answer after a flash: retry until the timeout.
bool wait_md1_ready(int timeout_ms, std::string* last, std::string* err) {
    const int64_t t0 = mono_ms();
    int64_t last_log = t0;
    std::string v, last_err;
    for (;;) {
        std::string e;
        v = md1_status(&e);
        if (v == "ready") {
            if (last) *last = v;
            return true;
        }
        if (!e.empty()) last_err = e;
        const int64_t now = mono_ms();
        if (now - t0 >= timeout_ms) break;
        if (now - last_log >= 5000) {
            log_note("md1.status = %s; waiting (%.0f s of %d)",
                     v.empty() ? (e.empty() ? "?" : e.c_str()) : v.c_str(), (now - t0) / 1000.0,
                     timeout_ms / 1000);
            last_log = now;
        }
        ::usleep(1000 * 1000);
    }
    if (last) *last = v;
    if (err)
        *err = "vendor.mtk.md1.status never reaches 'ready' (last value: " +
               (v.empty() ? std::string("?") : v) +
               (last_err.empty() ? "" : "; " + last_err) + ")";
    return false;
}

struct ModeResult {
    bool ok = false;
    std::string err;
    uint32_t stg = 0;
    int waited_ms = 0;
};

// Requests a mode from the modem and waits for it to be consumed (MDON == mode and
// MGO == 0). Cave modes must also close the return (STG == 14900).
ModeResult request_mode(Window& w, const ModeMap& map, int mode, int timeout_ms) {
    ModeResult r;
    if (mode <= 0) {
        r.err = "invalid mode";
        return r;
    }
    const uint32_t m = static_cast<uint32_t>(mode);
    const bool cave = mode >= map.cave_min;
    const int64_t t0 = mono_ms();

    if (!wit_write(w, kMarkMgo, 0) || !wit_write(w, kMarkRes, 0) ||
        !wit_write(w, kMarkStep, 0) || !wit_write(w, kMarkMdon, 0xFFFFFFFFu)) {
        r.err = "cannot write the command block (read-only window or out of range)";
        return r;
    }
    if (cave) {
        wit_write(w, kMarkXStg, 0);
        wit_write(w, kMarkX2Stg, 0);
    }
    if (!wit_write(w, kMarkMode, m) || !wit_write(w, kMarkMgo, 1)) {
        r.err = "cannot request mode " + std::to_string(mode);
        return r;
    }
    memory_barrier();

    int64_t last_poke = t0, last_md1 = t0;
    while (mono_ms() - t0 < timeout_ms) {
        ::usleep(40 * 1000);
        uint32_t mdon = 0, mgo = 1;
        if (!wit_read(w, kMarkMdon, &mdon) || !wit_read(w, kMarkMgo, &mgo)) {
            r.err = "lost the read of the command block";
            return r;
        }
        if (mdon == m && mgo == 0) {
            // The handshake only says "accepted": the firmware writes MDON and lowers
            // MGO BEFORE running the mode. The real end is marked by W_RES=3, which the
            // modem writes on return from the handler, cave modes included.
            bool done = false;
            while (mono_ms() < t0 + timeout_ms) {
                uint32_t res = 0;
                if (!wit_read(w, kMarkRes, &res)) {
                    r.err = "lost the read of the mode result";
                    return r;
                }
                if (res == 3) {
                    done = true;
                    break;
                }
                const int64_t now2 = mono_ms();
                if (now2 - last_md1 >= 1000) {
                    last_md1 = now2;
                    const std::string md1c = md1_status();
                    if (!md1c.empty() && md1c != "ready") {
                        r.err = "the modem stopped being ready during mode " +
                                std::to_string(mode) + " (md1.status = " + md1c + ")";
                        return r;
                    }
                }
                ::usleep(20 * 1000);
            }
            if (!done) {
                r.err = "mode " + std::to_string(mode) + " was accepted but did not finish in " +
                        std::to_string(timeout_ms) + " ms (W_RES other than 3)";
                return r;
            }
            if (!cave) {
                r.ok = true;
                r.waited_ms = static_cast<int>(mono_ms() - t0);
                return r;
            }
            const int64_t t1 = mono_ms();
            while (mono_ms() - t1 < 20000) {
                uint32_t stg = 0, stg2 = 0;
                wit_read(w, kMarkXStg, &stg);
                wit_read(w, kMarkX2Stg, &stg2);
                if (stg == kCaveDone || stg2 == kCaveDone) {
                    r.ok = true;
                    r.stg = stg;
                    r.waited_ms = static_cast<int>(mono_ms() - t0);
                    return r;
                }
                ::usleep(50 * 1000);
            }
            r.err = "the modem consumed mode " + std::to_string(mode) +
                    " but the cave did not close the return (STG other than 14900)";
            return r;
        }
        const int64_t now = mono_ms();
        if (now - last_poke >= 5000) {
            wit_write(w, kMarkMgo, 1);
            memory_barrier();
            last_poke = now;
        }
        if (now - last_md1 >= 1000) {
            last_md1 = now;
            const std::string md1 = md1_status();
            if (!md1.empty() && md1 != "ready") {
                r.err = "the modem stopped being ready during mode " + std::to_string(mode) +
                        " (md1.status = " + md1 + ")";
                return r;
            }
        }
    }
    r.err = "mode " + std::to_string(mode) + " was not served in " + std::to_string(timeout_ms) +
            " ms (SDR mode must be loaded; if not, enter first with --no-flash)";
    return r;
}

// Reads modem addresses through the modem table (the profile's read mode).
bool modem_peek(Window& w, const ModeMap& map, const std::vector<uint32_t>& addrs,
                std::vector<uint32_t>* vals, std::string* err) {
    vals->clear();
    if (map.peek_mode <= 0) {
        if (err) *err = "the profile does not declare a read mode";
        return false;
    }
    for (size_t i = 0; i < addrs.size(); i += kPeekMax) {
        const size_t n = std::min<size_t>(kPeekMax, addrs.size() - i);
        for (size_t k = 0; k < n; ++k) {
            if (!wit_write(w, kMarkHwa + static_cast<uint32_t>(4 * k), addrs[i + k]) ||
                !wit_write(w, kMarkHwr + static_cast<uint32_t>(4 * k), 0)) {
                if (err) *err = "cannot set up the read table";
                return false;
            }
        }
        if (!wit_write(w, kMarkHwn, static_cast<uint32_t>(n))) {
            if (err) *err = "cannot set the number of table entries";
            return false;
        }
        ModeResult r = request_mode(w, map, map.peek_mode, 20000);
        if (!r.ok) {
            if (err) *err = "the modem read failed: " + r.err;
            return false;
        }
        ::usleep(100 * 1000);
        for (size_t k = 0; k < n; ++k) {
            uint32_t v = 0;
            if (!wit_read(w, kMarkHwr + static_cast<uint32_t>(4 * k), &v)) {
                if (err) *err = "cannot read the table result";
                return false;
            }
            vals->push_back(v);
        }
    }
    return true;
}

// Writes addresses and reads them back (the profile's write mode). This is what the
// dump engine arming uses.
bool modem_poke(Window& w, const ModeMap& map, const std::vector<uint32_t>& addrs,
                const std::vector<uint32_t>& values, std::vector<uint32_t>* readback,
                std::string* err) {
    if (map.poke_mode <= 0) {
        if (err) *err = "the profile does not declare a write mode";
        return false;
    }
    if (addrs.size() != values.size() || addrs.empty() || addrs.size() > kPeekMax) {
        if (err) *err = "the write table takes between 1 and 32 pairs";
        return false;
    }
    for (size_t k = 0; k < addrs.size(); ++k) {
        if (!wit_write(w, kMarkHwa + static_cast<uint32_t>(4 * k), addrs[k]) ||
            !wit_write(w, kMarkHwv + static_cast<uint32_t>(4 * k), values[k]) ||
            !wit_write(w, kMarkHwr + static_cast<uint32_t>(4 * k), 0)) {
            if (err) *err = "cannot set up the write table";
            return false;
        }
    }
    if (!wit_write(w, kMarkHwn, static_cast<uint32_t>(addrs.size()))) {
        if (err) *err = "cannot set the number of table entries";
        return false;
    }
    ModeResult r = request_mode(w, map, map.poke_mode, 30000);
    if (!r.ok) {
        if (err) *err = "the modem write failed: " + r.err;
        return false;
    }
    ::usleep(100 * 1000);
    if (readback) readback->clear();
    for (size_t k = 0; k < addrs.size(); ++k) {
        uint32_t v = 0;
        if (!wit_read(w, kMarkHwr + static_cast<uint32_t>(4 * k), &v)) {
            if (err) *err = "cannot read the table readback";
            return false;
        }
        if (readback) readback->push_back(v);
        if (v != values[k]) {
            if (err)
                *err = "readback mismatch at " + hex32(addrs[k]) + ": wrote " +
                       hex32(values[k]) + ", read " + hex32(v);
            return false;
        }
    }
    return true;
}

// --- marker snapshots ------------------------------------------------------

struct PllInfo {
    bool valid = false;
    uint32_t raw0 = 0, raw1 = 0;
    unsigned channel = 0, band = 0;
    uint32_t khz = 0;
};

PllInfo read_pll(const Window& w) {
    PllInfo p;
    std::vector<uint32_t> v;
    if (!wit_block(w, kMarkPll, 16, &v) || v.size() < 3) return p;
    p.raw0 = v[0];
    p.raw1 = v[1];
    p.channel = (v[0] >> 8) & 0xFFu;      // byte +1: carrier
    p.band = (v[0] >> 16) & 0xFFFFu;      // half word +2: band
    p.khz = v[1];                          // word +4: kHz
    p.valid = true;
    return p;
}

bool pll_matches(const PllInfo& p, const ModeMap& m, std::string* why) {
    if (!p.valid) {
        if (why) *why = "cannot read the pll_channel_data copy (WIT+0x1B0)";
        return false;
    }
    if (p.channel != static_cast<unsigned>(m.pll_channel) ||
        p.band != static_cast<unsigned>(m.pll_band) || p.khz != m.pll_khz) {
        if (why)
            *why = "pll_channel_data does not hold what was seeded: channel " + std::to_string(p.channel) +
                   " / band " + std::to_string(p.band) + " / " + std::to_string(p.khz) +
                   " kHz (expected channel " + std::to_string(m.pll_channel) + " / band " +
                   std::to_string(m.pll_band) + " / " + std::to_string(m.pll_khz) + " kHz)";
        return false;
    }
    return true;
}

struct RailInfo {
    bool valid = false;
    uint32_t tag = 0;
    uint32_t con0[3] = {0, 0, 0};   // VRF18, VFE28, VRFCK
    uint32_t writes[3] = {0, 0, 0};
    uint32_t armed = 0, on = 0, per = 0, pass = 0, anom = 0, stop = 0;
    uint32_t tck = 0, tn = 0;
};

RailInfo read_rails(const Window& w) {
    RailInfo r;
    uint32_t tag = 0;
    if (!wit_read(w, kMarkX3Tag, &tag)) return r;
    r.tag = tag;
    r.valid = (tag == kTagR156);
    wit_read(w, kMarkX3V18, &r.con0[0]);
    wit_read(w, kMarkX3V28, &r.con0[1]);
    wit_read(w, kMarkX3Vck, &r.con0[2]);
    wit_read(w, kMarkX3W18, &r.writes[0]);
    wit_read(w, kMarkX3W28, &r.writes[1]);
    wit_read(w, kMarkX3Wck, &r.writes[2]);
    wit_read(w, kMarkX3Armed, &r.armed);
    wit_read(w, kMarkX3On, &r.on);
    wit_read(w, kMarkX3Per, &r.per);
    wit_read(w, kMarkX3Pass, &r.pass);
    wit_read(w, kMarkX3Anom, &r.anom);
    wit_read(w, kMarkX3Stop, &r.stop);
    wit_read(w, kMarkX3Tck, &r.tck);
    wit_read(w, kMarkX3Tn, &r.tn);
    return r;
}

struct TimingLoopInfo {
    bool valid = false;
    uint32_t tag = 0;
    uint32_t armed = 0, on = 0, pass = 0, neg = 0, bad = 0, stop = 0, force = 0, fpass = 0;
    uint32_t last[5] = {0, 0, 0, 0, 0};   // CDFFE[0], CDFFE[1], BSIE[0..2]
};

TimingLoopInfo read_timing_loop(const Window& w) {
    TimingLoopInfo t;
    uint32_t tag = 0;
    if (!wit_read(w, kMarkX4Tag, &tag)) return t;
    t.tag = tag;
    t.valid = (tag == kTagR159);
    wit_read(w, kMarkX4Armed, &t.armed);
    wit_read(w, kMarkX4On, &t.on);
    wit_read(w, kMarkX4Pass, &t.pass);
    wit_read(w, kMarkX4Neg, &t.neg);
    wit_read(w, kMarkX4Bad, &t.bad);
    wit_read(w, kMarkX4Stop, &t.stop);
    wit_read(w, kMarkX4Force, &t.force);
    wit_read(w, kMarkX4Fpass, &t.fpass);
    wit_read(w, kMarkX4V0, &t.last[0]);
    wit_read(w, kMarkX4V1, &t.last[1]);
    wit_read(w, kMarkX4V2, &t.last[2]);
    wit_read(w, kMarkX4V3, &t.last[3]);
    wit_read(w, kMarkX4V4, &t.last[4]);
    return t;
}

struct Beat {
    bool valid = false;
    uint32_t seq = 0, blks = 0, cps = 0, ucnt = 0;
};

Beat read_beat(const Window& w) {
    Beat b;
    if (!wit_read(w, kMarkSeq, &b.seq) || !wit_read(w, kMarkCps, &b.cps)) return b;
    wit_read(w, kMarkBlks, &b.blks);
    wit_read(w, kMarkCpsUcnt, &b.ucnt);
    b.valid = true;
    return b;
}

// ---------------------------------------------------------------------------
// Persisted state (/var/lib/sdr-load/state.json)
// ---------------------------------------------------------------------------

struct State {
    int version = 1;
    std::string state = "ok";     // ok | entering | sdr-active | flashing | restoring | recovering
    std::string pending;          // "" | reboot-to-enter | reboot-to-recover
    std::string updated;
    std::string image;
    std::string image_sha256;
    unsigned long long image_size = 0;
    std::string partition;
    std::string flash_script;
    bool wifi_blocked_by_tool = false;
    bool bt_blocked_by_tool = false;
    std::string last_error;
    std::string boot_id;
    // SDR mode activation (--activate / --deactivate).
    std::string mode_profile;     // profile it was activated with (name, path...)
    bool loop_armed = false;      // both loops are armed
    bool rails_on = false;        // the three CON0 were observed at 1

    bool loaded = false;  // not part of the file
    std::string loaded_from;  // file it was actually read from (not written)
};

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string json_find(const std::string& doc, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = doc.find(pat, pos)) != std::string::npos) {
        size_t p = pos + pat.size();
        while (p < doc.size() && std::isspace(static_cast<unsigned char>(doc[p]))) ++p;
        if (p >= doc.size() || doc[p] != ':') {
            pos = p;
            continue;
        }
        ++p;
        while (p < doc.size() && std::isspace(static_cast<unsigned char>(doc[p]))) ++p;
        if (p >= doc.size()) return "";
        if (doc[p] == '"') {
            ++p;
            std::string out;
            while (p < doc.size() && doc[p] != '"') {
                if (doc[p] == '\\' && p + 1 < doc.size()) {
                    char n = doc[p + 1];
                    if (n == 'n') out += '\n';
                    else if (n == 't') out += '\t';
                    else out += n;
                    p += 2;
                } else {
                    out += doc[p++];
                }
            }
            return out;
        }
        size_t e = p;
        while (e < doc.size() && doc[e] != ',' && doc[e] != '}' && doc[e] != '\n') ++e;
        return trim(doc.substr(p, e - p));
    }
    return "";
}

std::string current_boot_id() {
    std::string s;
    if (!read_file("/proc/sys/kernel/random/boot_id", &s)) return "";
    return trim(s);
}

// True if the state was written in a previous boot: the reboot already happened.
bool reboot_happened(const State& st) {
    if (st.boot_id.empty()) return false;
    const std::string cur = current_boot_id();
    return !cur.empty() && cur != st.boot_id;
}

// One canonical path for every caller. Root writes it and an unprivileged session
// reads the same file (0644). The old per-user fallback created a second, empty state
// the moment /var/lib was not writable, so the window reported "factory image" while
// the partition was in SDR mode and the real state said otherwise.
std::string default_state_path() {
    const char* env = std::getenv("SDR_LOAD_STATE");
    if (env && *env) return env;
    return kDefaultStatePath;
}

// Old versions kept a per-user copy when /var/lib was not writable. It is only read
// when the canonical file does not exist, it is never written, and the caller names it
// on screen: the next privileged write creates the canonical file, which wins from then
// on. That is the whole migration, and it needs no manual step.
std::string legacy_state_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return "";
    return std::string(home) + "/.local/state/sdr-load/state.json";
}

bool mkdir_p(const std::string& path) {
    if (path.empty() || path == "/") return true;
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) mkdir_p(path.substr(0, slash));
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool state_save(const std::string& path, const State& s, std::string* err) {
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !mkdir_p(path.substr(0, slash))) {
        if (err) *err = "cannot create " + path.substr(0, slash) + ": " + std::strerror(errno);
        return false;
    }
    if (slash != std::string::npos) {
        // The window reads this state without privileges: the directory needs 0755 and the
        // file 0644, whatever umask the privileged shell that runs the tool happens to have.
        // Otherwise a single state would exist and only root could see it.
        (void)::chmod(path.substr(0, slash).c_str(), 0755);
    }
    std::string tmp = path + ".tmp";
    std::string js;
    js += "{\n";
    js += "  \"version\": " + std::to_string(s.version) + ",\n";
    js += "  \"tool\": \"" + json_escape(kToolVersion) + "\",\n";
    js += "  \"state\": \"" + json_escape(s.state) + "\",\n";
    js += "  \"pending\": \"" + json_escape(s.pending) + "\",\n";
    js += "  \"updated\": \"" + json_escape(now_iso8601()) + "\",\n";
    js += "  \"image\": \"" + json_escape(s.image) + "\",\n";
    js += "  \"image_sha256\": \"" + json_escape(s.image_sha256) + "\",\n";
    js += "  \"image_size\": " + std::to_string(s.image_size) + ",\n";
    js += "  \"partition\": \"" + json_escape(s.partition) + "\",\n";
    js += "  \"flash_script\": \"" + json_escape(s.flash_script) + "\",\n";
    js += std::string("  \"wifi_blocked_by_tool\": ") + (s.wifi_blocked_by_tool ? "true" : "false") + ",\n";
    js += std::string("  \"bt_blocked_by_tool\": ") + (s.bt_blocked_by_tool ? "true" : "false") + ",\n";
    js += "  \"mode_profile\": \"" + json_escape(s.mode_profile) + "\",\n";
    js += std::string("  \"loop_armed\": ") + (s.loop_armed ? "true" : "false") + ",\n";
    js += std::string("  \"rails_on\": ") + (s.rails_on ? "true" : "false") + ",\n";
    js += "  \"last_error\": \"" + json_escape(s.last_error) + "\",\n";
    js += "  \"boot_id\": \"" + json_escape(s.boot_id.empty() ? current_boot_id() : s.boot_id) + "\"\n";
    js += "}\n";

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (err) *err = std::strerror(errno);
        return false;
    }
    // Readable by the window even under a restrictive umask: it is the only state.
    (void)::fchmod(fd, 0644);
    ssize_t n = ::write(fd, js.data(), js.size());
    if (n != static_cast<ssize_t>(js.size()) || ::fsync(fd) != 0) {
        if (err) *err = std::strerror(errno);
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        if (err) *err = std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// Reading the state has three outcomes, and they are not the same thing: a missing file
// means nothing has been done yet, an unreadable one means somebody must look at it with
// privileges. Neither of them is ever replaced by a default state.
enum class StateRead { Ok, Missing, Unreadable };

StateRead state_read_one(const std::string& path, State* s, std::string* err) {
    std::string doc;
    errno = 0;
    if (!read_file(path, &doc)) {
        const int e = errno != 0 ? errno : EIO;
        if (err) *err = std::strerror(e);
        return (e == ENOENT || e == ENOTDIR) ? StateRead::Missing : StateRead::Unreadable;
    }
    State t;
    t.state = json_find(doc, "state");
    t.pending = json_find(doc, "pending");
    t.updated = json_find(doc, "updated");
    t.image = json_find(doc, "image");
    t.image_sha256 = json_find(doc, "image_sha256");
    t.image_size = std::strtoull(json_find(doc, "image_size").c_str(), nullptr, 10);
    t.partition = json_find(doc, "partition");
    t.flash_script = json_find(doc, "flash_script");
    t.wifi_blocked_by_tool = json_find(doc, "wifi_blocked_by_tool") == "true";
    t.bt_blocked_by_tool = json_find(doc, "bt_blocked_by_tool") == "true";
    t.mode_profile = json_find(doc, "mode_profile");
    t.loop_armed = json_find(doc, "loop_armed") == "true";
    t.rails_on = json_find(doc, "rails_on") == "true";
    t.last_error = json_find(doc, "last_error");
    t.boot_id = json_find(doc, "boot_id");
    if (t.state.empty()) {
        if (err) *err = "the file does not hold an sdr-load state (no \"state\" key)";
        return StateRead::Unreadable;
    }
    t.loaded = true;
    t.loaded_from = path;
    *s = t;
    return StateRead::Ok;
}

StateRead state_read(const std::string& path, State* s, std::string* err) {
    const StateRead r = state_read_one(path, s, err);
    if (r != StateRead::Missing || path != kDefaultStatePath) return r;
    // A file left by an old version in the user's home is a read fallback only: it is
    // used when the canonical one does not exist, it is never written, and the caller
    // says where the state came from.
    const std::string legacy = legacy_state_path();
    if (legacy.empty() || legacy == path) return r;
    if (state_read_one(legacy, s, nullptr) != StateRead::Ok) return r;
    return StateRead::Ok;
}

bool state_load(const std::string& path, State* s, std::string* err) {
    return state_read(path, s, err) == StateRead::Ok;
}

// ---------------------------------------------------------------------------
// Modem: presence in ofono and AT response (best effort, short timeout)
// ---------------------------------------------------------------------------

bool ofono_modem_present(std::string* out) {
    if (!command_exists("busctl")) return false;
    return run_capture({"busctl", "--timeout=3", "call", "org.ofono", "/ril_0", "org.ofono.Modem",
                        "GetProperties"},
                       out) == 0;
}

bool at_responds(std::string* out, int attempts = 2) {
    if (!command_exists("busctl")) return false;
    const std::vector<std::string> argv = {"busctl", "--timeout=6", "call", "org.ofono",
                                           "/ril_0", "org.ofono.FuriLabs.AT", "SendCommand",
                                           "s", "\"AT\""};
    std::string buf;
    for (int i = 0; i < attempts; ++i) {
        if (run_capture(argv, &buf) == 0 && buf.find("OK") != std::string::npos) {
            if (out) *out = buf;
            return true;
        }
        ::usleep(500 * 1000);
    }
    if (out) *out = buf;
    return false;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

enum class Action {
    Check,
    Load,
    Flash,
    Restore,
    Status,
    VerifyBoot,
    SelfTest,
    Activate,
    Resume,
    Deactivate,
    Modes,
    Help
};

struct Options {
    Action action = Action::Check;
    std::string image;
    std::string no_flash_sha256;
    std::string partition;
    std::string flash_script = kDefaultFlashScript;
    std::string recover_image = kDefaultRecoverImage;
    std::string state_path;
    std::string image_sha256_expected;
    unsigned long long image_size = 0;  // usable bytes of the partition (0 = whole)
    std::string mode_profile;           // --profile: known name or path
    bool reboot = false;
    bool no_reboot = false;
    bool assume_yes = false;
    bool dry_run = false;
    bool json = false;
    bool quiet = false;
    int settle_ms = 1500;
    int gap_ms = 1000;
    int samples = 2;
    // ENTER_DEEP_FLIGHT does not stop the modem instantly: the rings reach zero around six
    // seconds after the ioctl (measured). This is the deadline for waiting on them.
    int quiet_timeout_ms = 20000;
    int wait_ms = 20000;
    int md1_timeout_ms = 60000;   // the modem takes >20 s to answer
    int mode_timeout_ms = 90000;  // per mode (the cave can take a while)
    int beat_ms = 2000;           // separation between the two ring samples
    // The modem publishes in bursts (60-200 captures over 10-45 s, then silence): the
    // dump-engine mark is looked for during this window before calling it missing. A
    // single look can fall in a silence and report "no dump" about a working setup.
    int mark_timeout_ms = 45000;
    bool allow_unverified = false;
    bool via_resume = false;  // --resume delegating into --activate (header only)
};

// ---------------------------------------------------------------------------
// Exit codes
// ---------------------------------------------------------------------------

enum Rc {
    RC_OK = 0,
    RC_USAGE = 1,
    RC_PRECONDITION = 2,
    RC_RADIO_BLOCK = 3,
    RC_FLASH_FAILED = 4,
    RC_SHA_MISMATCH = 5,
    RC_REBOOT_NEEDED = 6,
    RC_IOCTL_FAILED = 7,
    RC_MODE_UNVERIFIED = 8,
    RC_RESTORE_INCOMPLETE = 9,
    RC_RECOVERY_FLASHED = 10,
    RC_REBOOTING = 11,
    RC_NO_DUMP = 12,
    RC_NO_PROFILE = 13,
    RC_ACTIVATE_FAILED = 14,
};

// ---------------------------------------------------------------------------
// Plans (printed by --dry-run and exercised by --selftest)
// ---------------------------------------------------------------------------

std::vector<std::string> plan_load(const Options& o, const State& st) {
    std::vector<std::string> p;
    p.push_back("check privileges and device nodes");
    if (st.loaded && st.pending == "reboot-to-enter")
        p.push_back("ABORT: a flash is pending reboot");
    p.push_back("block WiFi and Bluetooth through rfkill and record what we change");
    p.push_back("check that no ccmni* or wlan* interface is left up");
    if (!o.no_flash_sha256.empty())
        p.push_back("verify the partition sha256 against " + o.no_flash_sha256);
    p.push_back("enter SDR mode (ioctl 0x430E) and save state sdr-active");
    p.push_back("wait " + std::to_string(o.settle_ms) + " ms and require quiet rings");
    p.push_back("look for the dump-engine mark during " + std::to_string(o.mark_timeout_ms) +
                " ms (the modem publishes in bursts) and count data words");
    p.push_back("report what is missing (dump not programmed / no data)");
    return p;
}

std::vector<std::string> plan_flash(const Options& o) {
    std::vector<std::string> p;
    p.push_back("block radios (WiFi, Bluetooth)");
    p.push_back("compute the sha256 of " + o.image);
    p.push_back("save state flashing BEFORE writing");
    p.push_back("run " + o.flash_script + " " + o.image);
    p.push_back("verify the sha256 of the written partition");
    p.push_back("save state ok with pending=reboot-to-enter");
    if (o.reboot) p.push_back("reboot the phone (systemctl reboot)");
    else p.push_back("ask for a manual reboot and exit with code 6");
    return p;
}

std::vector<std::string> plan_restore(const Options& o, const State& st) {
    std::vector<std::string> p;
    p.push_back("save state restoring BEFORE touching anything");
    p.push_back("leave SDR mode (ioctl 0x430F)");
    p.push_back("unblock through rfkill only what we blocked"
                + std::string(st.loaded ? "" : " (no state: everything is unblocked)"));
    p.push_back("restart ofono and wait for the modem (ril_0 object + AT), up to " +
                std::to_string(o.wait_ms) + " ms");
    p.push_back("if the modem does not come back: state recovering and flash " + o.recover_image);
    p.push_back("verify the sha256 of the recovery image");
    if (o.no_reboot) p.push_back("do not reboot: exit with code 10 and instructions");
    else if (o.reboot) p.push_back("if it still does not answer: reboot (code 11)");
    else p.push_back("if it still does not answer: ask for confirmation; no tty, code 10");
    p.push_back("close with state ok and a summary");
    return p;
}

std::vector<std::string> plan_activate(const Options& o, const ModeMap& m) {
    std::vector<std::string> p;
    p.push_back("identify the partition image by sha256 and pick a profile (" + m.name + ")");
    p.push_back("if the state says pending=reboot-to-enter: check the partition against the recorded "
                "sha256 and close it WITHOUT writing the image again");
    p.push_back("if SDR mode is not entered yet: block the radios and enter it (ioctl 0x430E), as a "
                "resumed --no-flash would");
    p.push_back("wait for vendor.mtk.md1.status = ready, re-reading every second until " +
                std::to_string(o.md1_timeout_ms) + " ms (the modem takes >20 s to answer)");
    if (m.arm_addr != 0)
        p.push_back("arm the dump engine: " + hex32(m.arm_addr) + " <- " + hex32(m.arm_value) +
                    " (mode " + std::to_string(m.poke_mode) + ") and require the readback");
    else
        p.push_back("the profile does not declare arming: the dump is assumed already armed");
    p.push_back("require the dump-engine mark within " + std::to_string(o.mark_timeout_ms) +
                " ms (written in bursts: one look can fall in a silence)");
    p.push_back("seed the PLL (mode " + std::to_string(m.pll_seed) + ") and require channel " +
                std::to_string(m.pll_channel) + ", band " + std::to_string(m.pll_band) + ", " +
                std::to_string(m.pll_khz) + " kHz in pll_channel_data");
    p.push_back("power the rails one at a time (modes " + std::to_string(m.rail_v18) + "/" +
                std::to_string(m.rail_v28) + "/" + std::to_string(m.rail_vck) +
                "): CON0 |= 1, never 0, and check the readback of each one");
    p.push_back("arm the rail keepalive (mode " + std::to_string(m.keepalive) + ", period " +
                std::to_string(m.loop_period) + ") and require X3_ARMED = KPL1 with PASS >= 1");
    p.push_back("arm the timing table (mode " + std::to_string(m.timing) + ") and require X4_ARMED = KPL2, " +
                "timing table A = " + std::to_string(m.cdf0) + "," + std::to_string(m.cdf1) +
                " and timing table B = " + std::to_string(m.bsi) + " x3");
    p.push_back("sample the ring (" + std::to_string(o.beat_ms) +
                " ms) and require seq or the copy counter to advance; re-read the PLL and the rails");
    p.push_back("if any step fails: stop the loops (mode " + std::to_string(m.loop_stop) +
                ") and leave the state at activate-failed");
    return p;
}

std::vector<std::string> plan_deactivate(const ModeMap& m) {
    std::vector<std::string> p;
    p.push_back("identify the image and the profile stop mode (" + m.name + ")");
    p.push_back("request mode " + std::to_string(m.loop_stop) +
                ": stops both loops and does not touch the PMIC");
    p.push_back("require X3_ARMED = 0 and X4_ARMED = 0 (and X3_STOP/X4_STOP to rise)");
    p.push_back("read CON0 of VRF18/VFE28/VRFCK and record that they are NOT powered down in software");
    p.push_back("save the state with loop_armed = false");
    p.push_back("to go all the way back (radios and SDR mode): sdr-load --restore");
    return p;
}

// ---------------------------------------------------------------------------
// Device utilities
// ---------------------------------------------------------------------------

bool running_as_root() { return ::geteuid() == 0; }

// The flash script is the user's (by default the phone's own /home/furios/flash_modem.sh)
// or the one `make install` leaves in the system tree. The first readable one wins, so a
// checkout next to the tool works without installing anything.
std::string flash_script_path(const Options& o) {
    if (::access(o.flash_script.c_str(), R_OK) == 0) return o.flash_script;
    if (::access(kInstalledFlashScript, R_OK) == 0) return kInstalledFlashScript;
    return o.flash_script;
}

bool partition_from_script(const std::string& script, std::string* out) {
    std::string doc;
    if (!read_file(script, &doc)) return false;
    size_t pos = 0;
    while (pos < doc.size()) {
        size_t eol = doc.find('\n', pos);
        if (eol == std::string::npos) eol = doc.size();
        std::string line = trim(doc.substr(pos, eol - pos));
        if (line.compare(0, 5, "PART=") == 0) {
            std::string v = line.substr(5);
            if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
                char q = v[0];
                size_t end = v.find(q, 1);
                if (end != std::string::npos) v = v.substr(1, end - 1);
            }
            v = trim(v);
            if (!v.empty()) {
                if (out) *out = v;
                return true;
            }
        }
        pos = eol + 1;
    }
    return false;
}

// The partition is the one from the command line, else the one the flash script names,
// else the one the state recorded when it was flashed. The last one is only a fallback:
// it lets an unprivileged --status still look at the partition it can read, and a
// moved/renamed script does not turn the report into "unknown".
bool resolve_partition(const Options& o, std::string* out, std::string* err,
                       const State* st = nullptr) {
    if (!o.partition.empty()) {
        *out = o.partition;
        return true;
    }
    if (partition_from_script(flash_script_path(o), out)) return true;
    if (st && !st->partition.empty()) {
        *out = st->partition;
        return true;
    }
    if (err) *err = "cannot infer the partition from the flash script; use --partition";
    return false;
}

// Usable bytes of the partition: the image size, not the whole partition size.
uint64_t expected_image_size(const Options& o, const State& st, bool* from_state = nullptr) {
    if (o.image_size != 0) return o.image_size;
    if (st.loaded && st.image_size != 0) {
        if (from_state) *from_state = true;
        return st.image_size;
    }
    return 0;
}

std::string human_bytes(uint64_t n) {
    char buf[64];
    if (n >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.2f GiB", n / double(1ull << 30));
    else if (n >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.2f MiB", n / double(1ull << 20));
    else if (n >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KiB", n / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
    return buf;
}

// ---------------------------------------------------------------------------
// Image -> mode profile
//
// It is identified by the sha256 of the partition's usable bytes (the image
// bytes). If it is not in the table there is no activation: mode numbers from
// another image may do something else.
// ---------------------------------------------------------------------------

struct ImageId {
    bool readable = false;
    uint64_t limit = 0;   // bytes hashed (0 = whole partition)
    uint64_t size = 0;
    std::string sha256;
    std::string partition;
    std::string err;
};

ImageId identify_partition(const Options& o, const State& st) {
    ImageId id;
    std::string err;
    if (!resolve_partition(o, &id.partition, &err, &st)) {
        id.err = err;
        return id;
    }
    id.limit = expected_image_size(o, st);
    std::string hex;
    uint64_t size = 0;
    if (!sha256_of_file(id.partition, id.limit, &hex, &size, &err)) {
        id.err = "cannot read " + id.partition + ": " + err;
        return id;
    }
    id.sha256 = hex;
    id.size = size;
    id.readable = true;
    return id;
}

// ---------------------------------------------------------------------------
// What the partition really holds
//
// This is the only truth about the image that is loaded. The state file can say
// "sdr-active" for a partition that was written back to stock by a restore, a
// recovery or a flash from another terminal: from then on the state is a lie, and
// acting on it (arming a modem whose image has no dump engine) only wastes time.
// ---------------------------------------------------------------------------

struct PartitionFacts {
    bool resolved = false;   // the partition path is known
    bool readable = false;   // it could be hashed
    std::string partition;
    uint64_t limit = 0;      // bytes hashed (0 = the whole partition)
    uint64_t size = 0;
    std::string sha256;
    std::string err;
    std::string profile;     // profile this image maps to ("" = none)
    bool stock = false;      // it is the recovery image
    std::string stock_path;  // recovery image it matches
    bool whole = false;      // the image size is unknown: the whole partition was hashed
};

// First 16 hex digits: enough to tell two images apart on one line, short enough to
// keep a sentence readable. The full sha256 is printed next to it by --status.
std::string short_sha(const std::string& sha) {
    if (sha.empty()) return "?";
    return sha.size() <= 16 ? sha : sha.substr(0, 16) + "...";
}

// sha256 of the recovery image, hashed once per process: it is the reference for
// "the partition holds the stock image". A missing or unrelated file is not an error:
// then the stock image simply cannot be named, and the image is described by its sha.
bool stock_image_sha(const std::string& path, std::string* sha, uint64_t* size) {
    static std::string cached_path, cached_sha;
    static uint64_t cached_size = 0;
    static bool cached = false;
    if (cached && cached_path == path) {
        if (sha) *sha = cached_sha;
        if (size) *size = cached_size;
        return !cached_sha.empty();
    }
    struct stat st {};
    if (path.empty() || ::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    std::string hex, err;
    uint64_t n = 0;
    if (!sha256_of_file(path, 0, &hex, &n, &err)) return false;
    cached = true;
    cached_path = path;
    cached_sha = hex;
    cached_size = n;
    if (sha) *sha = hex;
    if (size) *size = n;
    return true;
}

PartitionFacts partition_facts(const Options& o, const State& st) {
    PartitionFacts f;
    std::string err;
    if (!resolve_partition(o, &f.partition, &err, &st)) {
        f.err = err;
        return f;
    }
    f.resolved = true;
    f.limit = expected_image_size(o, st);
    f.whole = f.limit == 0;
    std::string hex;
    uint64_t size = 0;
    if (!sha256_of_file(f.partition, f.limit, &hex, &size, &err)) {
        f.err = "cannot read " + f.partition + ": " + err;
        return f;
    }
    f.sha256 = hex;
    f.size = size;
    f.readable = true;
    // Every image in the table is exactly kStockImageSize bytes. With no --size and no
    // state the whole partition was hashed; hash that prefix once and identify the image
    // from it, so a partition flashed by hand (no state file) is still usable.
    if (f.whole && size >= kStockImageSize) {
        std::string prefix;
        uint64_t prefix_size = 0;
        if (sha256_of_file(f.partition, kStockImageSize, &prefix, &prefix_size, &err)) {
            const ImageProfile* p = find_profile_by_sha256(prefix);
            if (p || prefix == kStockSha256) {
                f.whole = false;
                f.limit = kStockImageSize;
                f.sha256 = prefix;
                f.size = prefix_size;
                if (p) f.profile = p->name;
            }
            if (prefix == kStockSha256) {
                f.stock = true;
                if (::access(o.recover_image.c_str(), R_OK) == 0) f.stock_path = o.recover_image;
            }
        }
    } else if (!f.whole) {
        const ImageProfile* p = find_profile_by_sha256(hex);
        if (p) f.profile = p->name;
    }
    if (!f.stock && !f.whole && f.sha256 == kStockSha256) {
        f.stock = true;
        if (::access(o.recover_image.c_str(), R_OK) == 0) f.stock_path = o.recover_image;
    }
    std::string stock_sha;
    uint64_t stock_size = 0;
    if (!f.stock && stock_image_sha(o.recover_image, &stock_sha, &stock_size) && !f.whole &&
        stock_size == f.size && stock_sha == f.sha256) {
        f.stock = true;
        f.stock_path = o.recover_image;
    }
    return f;
}

// "mode image standard-a", "stock image", ...: the noun phrase used in every message.
std::string image_kind_text(const PartitionFacts& f) {
    if (!f.resolved) return "unknown (the partition is not known)";
    if (!f.readable) return "unreadable";
    if (!f.profile.empty()) return "mode image " + f.profile;
    if (f.stock) return "stock image";
    if (f.whole) return "the whole partition was hashed (the image size is unknown)";
    return "no profile matches this image";
}

enum class ImageVerdict { Unknown, Coherent, Incoherent, Unverified };

struct ImageTruth {
    ImageVerdict verdict = ImageVerdict::Unknown;
    bool state_claims_sdr = false;
    bool matches_state = false;
    std::string text;  // one line, what --status prints after "image check:"
    std::string hint;  // the next step when the state and the partition disagree
};

bool state_claims_sdr(const State& st) {
    return st.state == "sdr-active" || st.state == "activating" ||
           st.state == "activate-failed" || st.state == "entering";
}

// Compares three things that should always agree: the partition, the sha256 the state
// recorded and the profile table. The state never wins: if it says SDR mode is loaded
// and the partition holds the stock image, the message says exactly that.
ImageTruth image_truth(const State& st, const PartitionFacts& f) {
    ImageTruth t;
    t.state_claims_sdr = state_claims_sdr(st);
    t.matches_state = f.readable && !f.whole && !st.image_sha256.empty() &&
                      f.sha256 == st.image_sha256 &&
                      (st.image_size == 0 || f.size == st.image_size);
    if (!f.readable) {
        t.verdict = ImageVerdict::Unverified;
        t.text = "unverified: " +
                 (f.err.empty() ? std::string("the partition was not read") : f.err);
        t.hint = "run it with privileges to read the partition: sudo sdr-load --status";
        return t;
    }
    if (f.whole) {
        t.verdict = ImageVerdict::Unverified;
        t.text = "unverified: the image size is unknown and the whole partition was hashed"
                 " (" + short_sha(f.sha256) + ")";
        t.hint = "pass --size <bytes> (the image ones) so the partition can be compared";
        return t;
    }
    const std::string kind = image_kind_text(f);
    if (t.state_claims_sdr && f.profile.empty()) {
        t.verdict = ImageVerdict::Incoherent;
        t.text = "INCOHERENT: state says " + st.state + " but the partition holds " +
                 short_sha(f.sha256) + " (" + kind + ")";
        if (!st.image_sha256.empty() && !t.matches_state)
            t.text += "; the state recorded " + short_sha(st.image_sha256);
        t.hint = "the image was written back: write the mode image again with "
                 "sdr-load --image <path> (reboot afterwards)";
        return t;
    }
    if (t.state_claims_sdr && !st.image_sha256.empty() && !t.matches_state) {
        t.verdict = ImageVerdict::Incoherent;
        t.text = "INCOHERENT: state says " + st.state + " but the partition holds " +
                 short_sha(f.sha256) + " (" + kind + ") and the state recorded " +
                 short_sha(st.image_sha256);
        t.hint = "the partition changed behind the state: write the mode image again with "
                 "sdr-load --image <path> (reboot afterwards)";
        return t;
    }
    t.verdict = ImageVerdict::Coherent;
    if (!f.profile.empty()) {
        t.text = "OK the partition holds " + kind;
        if (t.state_claims_sdr && t.matches_state)
            t.text += ", the image the state recorded";
    } else if (f.stock) {
        t.text = "OK the partition holds the stock image (no SDR mode loaded)";
    } else if (st.image_sha256.empty()) {
        t.text = "the state records no image; the partition holds " + short_sha(f.sha256) +
                 " (" + kind + ")";
        t.verdict = ImageVerdict::Unknown;
    } else {
        t.text = "the partition holds " + short_sha(f.sha256) + " (" + kind + ")";
        t.verdict = ImageVerdict::Unknown;
    }
    return t;
}

// ---------------------------------------------------------------------------
// The dump-engine mark is written in bursts
//
// The modem publishes 60-200 captures over 10-45 s and then goes silent for a while.
// The mark (kWinMark == "NRDQ") only exists while a burst is running, so a single look
// can fall in a silence and report "no dump" about a setup that works. It is looked
// for during a window instead, and only then called missing.
// ---------------------------------------------------------------------------

struct MarkProbe {
    bool ok = false;
    bool window = false;
    int waited_ms = 0;
    int polls = 0;
    std::string err;
};

// Looks for the mark on a window that is already open (the activation keeps one open
// for the whole sequence: the node cannot be opened twice from the same process by
// accident, and reusing the handle is also faster).
MarkProbe wait_dump_mark(Window& w, int timeout_ms, int poll_ms = 500) {
    MarkProbe r;
    r.window = w.valid();
    if (!r.window) {
        r.err = "the window is not open";
        return r;
    }
    const int64_t t0 = mono_ms();
    for (;;) {
        ++r.polls;
        if (w.rd32(kWinMark) == kMarkMagic) {
            r.ok = true;
            break;
        }
        if (mono_ms() - t0 >= timeout_ms) break;
        ::usleep(static_cast<useconds_t>(poll_ms) * 1000);
    }
    r.waited_ms = static_cast<int>(mono_ms() - t0);
    return r;
}

// Opens its own window and looks for the mark there.
MarkProbe probe_dump_mark(int timeout_ms, int poll_ms = 500) {
    MarkProbe r;
    Window w;
    if (!w.open(&r.err)) return r;
    r = wait_dump_mark(w, timeout_ms, poll_ms);
    return r;
}

// Why the mark is missing: the two causes are different failures and do not share a
// message. RC_NO_DUMP (12) means "the image is the right one but the engine is not
// publishing"; RC_SHA_MISMATCH (5) means "the partition does not hold the mode image".
struct MarkDiagnosis {
    int rc = RC_NO_DUMP;
    std::string text;
};

MarkDiagnosis diagnose_missing_mark(const PartitionFacts& f, const std::string& want_sha,
                                    const std::string& profile, int timeout_ms,
                                    const std::string& tail) {
    MarkDiagnosis d;
    const int secs = timeout_ms / 1000;
    const bool known = f.readable && !f.whole;
    bool wrong = false;
    if (known) {
        if (!want_sha.empty()) wrong = f.sha256 != want_sha;
        else if (!f.profile.empty() && !profile.empty()) wrong = f.profile != profile;
        else wrong = f.stock;  // a stock image is never a mode one
    }
    if (wrong) {
        d.rc = RC_SHA_MISMATCH;
        d.text = "the image is not in place: the partition holds " + short_sha(f.sha256) +
                 " (" + image_kind_text(f) + ")";
        if (!profile.empty()) d.text += ", not the image of profile " + profile;
        d.text += "; write the mode image again with sdr-load --image <path>";
        return d;
    }
    d.rc = RC_NO_DUMP;
    d.text = "the image is correct";
    if (!profile.empty()) d.text += " (profile " + profile + ")";
    if (!f.readable) d.text += " as far as it could be checked here (" + f.err + ")";
    d.text += " but the dump-engine mark (WIT+0x40000) did not appear in " +
              std::to_string(secs) + " s";
    if (!tail.empty()) d.text += ": " + tail;
    return d;
}

bool resolve_mode_map(const Options& o, const State& st, ModeMap* out, std::string* err,
                      bool* identified, ImageId* id_out = nullptr,
                      const PartitionFacts* facts = nullptr) {
    if (identified) *identified = false;

    ImageId id;
    if (facts) {
        id.partition = facts->partition;
        id.limit = facts->limit;
        id.size = facts->size;
        id.sha256 = facts->sha256;
        id.readable = facts->readable;
        id.err = facts->err;
    } else {
        id = identify_partition(o, st);
    }
    if (id_out) *id_out = id;
    if (id.readable && !o.image_sha256_expected.empty() && id.limit != 0 &&
        id.sha256 != o.image_sha256_expected) {
        if (err)
            *err = "the partition has sha256 " + id.sha256 + " and " +
                   o.image_sha256_expected + " was expected";
        return false;
    }

    if (!o.mode_profile.empty()) {
        const bool looks_path = o.mode_profile.find('/') != std::string::npos ||
                                o.mode_profile.find(".conf") != std::string::npos ||
                                o.mode_profile.find(".txt") != std::string::npos;
        if (looks_path) {
            if (!load_profile_file(o.mode_profile, out, err)) return false;
        } else {
            const ImageProfile* p = find_profile_by_name(o.mode_profile);
            if (!p) {
                if (err)
                    *err = "unknown profile: '" + o.mode_profile + "' (known: " +
                           known_profile_names() + "; or pass a path to a profile file)";
                return false;
            }
            *out = map_from_profile(*p);
        }
        // The profile sha256, if it declares one, must be the partition one.
        if (!out->sha256.empty()) {
            if (!id.readable) {
                log_warn("cannot check the partition sha256 (%s); profile %s is used as is",
                         id.err.c_str(), out->name.c_str());
            } else if (id.limit == 0) {
                log_warn("the number of partition bytes that make up the image is unknown (--size or state): "
                         "the sha256 of profile %s is not checked", out->name.c_str());
            } else if (id.sha256 != out->sha256) {
                if (err)
                    *err = "the partition has sha256 " + id.sha256 + " and profile " + out->name +
                           " expects " + out->sha256;
                return false;
            } else if (identified) {
                *identified = true;
            }
        }
        std::string why;
        if (!out->complete(&why)) {
            if (err) *err = why;
            return false;
        }
        return true;
    }

    if (!id.readable) {
        if (err)
            *err = "cannot identify the partition image (" + id.err +
                   "); pass --profile <name|file> or --partition/--size";
        return false;
    }
    if (id.limit == 0) {
        if (err)
            *err = "the number of bytes of " + id.partition +
                   " that make up the image is unknown (--size or state): cannot compare with the table";
        return false;
    }
    const ImageProfile* p = find_profile_by_sha256(id.sha256);
    if (!p) {
        if (err) {
            *err = "the partition image (sha256 " + id.sha256 +
                   ") is not in the profile table: modes are not requested blindly";
            for (const auto& k : known_profiles())
                *err += "\n  known: " + k.name + " (" + k.image_hint + ", " + k.sha256 + ")";
        }
        return false;
    }
    *out = map_from_profile(*p);
    if (identified) *identified = true;
    std::string why;
    if (!out->complete(&why)) {
        if (err) *err = why;
        return false;
    }
    return true;
}

struct RadiosReport {
    std::vector<RfkillEntry> entries;
    bool wifi_ok = true;
    bool bt_ok = true;
    std::vector<std::string> ccmni_up;
    std::vector<std::string> wlan_up;
    std::string detail;
};

RadiosReport radios_report() {
    RadiosReport r;
    r.entries = rfkill_scan();
    bool saw_wifi = false, saw_bt = false;
    for (const auto& e : r.entries) {
        if (e.type == "wlan") {
            saw_wifi = true;
            if (!e.soft) r.wifi_ok = false;
        } else if (e.type == "bluetooth") {
            saw_bt = true;
            if (!e.soft) r.bt_ok = false;
        }
    }
    if (!saw_wifi) r.wifi_ok = true;
    if (!saw_bt) r.bt_ok = true;
    r.ccmni_up = up_interfaces_with_prefix("ccmni");
    r.wlan_up = up_interfaces_with_prefix("wlan");
    return r;
}

bool print_radio_table(const RadiosReport& r) {
    if (r.entries.empty()) {
        log_note("rfkill: no entries (there are no radios to block)");
    } else {
        for (const auto& e : r.entries) {
            log_note("rfkill: %-10s %-8s soft=%s hard=%s", e.name.c_str(), e.type.c_str(),
                     e.soft ? "yes" : "no", e.hard ? "yes" : "no");
        }
    }
    log_note("ccmni* interfaces up: %s",
             r.ccmni_up.empty() ? "none" : join(r.ccmni_up, ", ").c_str());
    log_note("wlan* interfaces up : %s",
             r.wlan_up.empty() ? "none" : join(r.wlan_up, ", ").c_str());
    return r.wifi_ok && r.bt_ok && r.ccmni_up.empty() && r.wlan_up.empty();
}

// Disables WiFi and Bluetooth through rfkill (sysfs: the rfkill binary is not needed,
// and it is not installed on the phone). Returns 0 if everything ended up blocked.
int block_radios(State* st, bool dry_run) {
    RadiosReport before = radios_report();
    for (auto& e : before.entries) {
        if (e.type != "wlan" && e.type != "bluetooth") continue;
        if (e.soft) continue;  // already blocked before us: not recorded
        if (dry_run) {
            log_note("(dry-run) would block %s (%s)", e.name.c_str(), e.type.c_str());
            continue;
        }
        RfkillEntry tmp = e;
        if (!rfkill_set_soft(tmp, true)) {
            log_error("could not block %s (%s): %s", e.name.c_str(), e.type.c_str(),
                      std::strerror(errno));
            return RC_RADIO_BLOCK;
        }
        if (e.type == "wlan") st->wifi_blocked_by_tool = true;
        if (e.type == "bluetooth") st->bt_blocked_by_tool = true;
        log_note("%s (%s) blocked", e.name.c_str(), e.type.c_str());
    }
    if (dry_run) return 0;

    // WiFi interfaces should disappear when blocked; if any is still up, take it down.
    RadiosReport after = radios_report();
    for (const auto& name : after.wlan_up) {
        log_warn("%s is still up after blocking WiFi; taking it down", name.c_str());
        run_inherit({"ip", "link", "set", name, "down"});
    }
    after = radios_report();
    if (!after.wifi_ok || !after.bt_ok || !after.wlan_up.empty()) {
        log_error("the radios are still active (wifi=%s bt=%s wlan_up=%s)",
                  after.wifi_ok ? "blocked" : "ON", after.bt_ok ? "blocked" : "ON",
                  after.wlan_up.empty() ? "none" : join(after.wlan_up, ",").c_str());
        return RC_RADIO_BLOCK;
    }
    return 0;
}

int unblock_radios(const State& st, bool dry_run) {
    int failures = 0;
    for (auto& e : rfkill_scan()) {
        bool ours = (e.type == "wlan" && st.wifi_blocked_by_tool) ||
                    (e.type == "bluetooth" && st.bt_blocked_by_tool);
        if (!st.loaded && (e.type == "wlan" || e.type == "bluetooth")) ours = true;  // no state
        if (!ours || !e.soft) continue;
        if (dry_run) {
            log_note("(dry-run) would unblock %s (%s)", e.name.c_str(), e.type.c_str());
            continue;
        }
        RfkillEntry tmp = e;
        if (!rfkill_set_soft(tmp, false)) {
            log_warn("could not unblock %s (%s): %s", e.name.c_str(), e.type.c_str(),
                     std::strerror(errno));
            ++failures;
        } else {
            log_note("%s (%s) unblocked", e.name.c_str(), e.type.c_str());
        }
    }
    return failures == 0 ? 0 : 1;
}

int enter_sdr_mode(int* ioctl_errno) {
    int fd = ::open(kIoctlNode, O_RDWR);
    if (fd < 0) {
        if (ioctl_errno) *ioctl_errno = errno;
        return -1;
    }
    int r = ::ioctl(fd, kIoctlEnterSdr, 0);
    if (ioctl_errno) *ioctl_errno = errno;
    ::close(fd);
    return r;
}

int exit_sdr_mode(int* ioctl_errno) {
    int fd = ::open(kIoctlNode, O_RDWR);
    if (fd < 0) {
        if (ioctl_errno) *ioctl_errno = errno;
        return -1;
    }
    int r = ::ioctl(fd, kIoctlExitSdr, 0);
    if (ioctl_errno) *ioctl_errno = errno;
    ::close(fd);
    return r;
}

bool confirm(const Options& o, const std::string& question) {
    if (o.assume_yes) return true;
    if (!::isatty(STDIN_FILENO)) return false;
    std::printf("%s [y/N] ", question.c_str());
    std::fflush(stdout);
    char buf[16] = {0};
    if (!std::fgets(buf, sizeof(buf), stdin)) return false;
    std::string a = trim(buf);
    return a == "y" || a == "Y" || a == "yes" || a == "YES" || a == "s" || a == "si";
}

struct ModeReport {
    bool window_ok = false;
    std::string window_err;
    RingDiff rings;
    bool rings_ok = false;
    bool mark = false;
    bool mark2 = false;
    uint64_t data_words = 0;
    uint32_t window_base = 0;
};

ModeReport inspect_mode(const Options& o, bool sample) {
    ModeReport m;
    Window w;
    if (!w.open(&m.window_err)) {
        m.window_ok = false;
        return m;
    }
    m.window_ok = true;
    m.window_base = w.base();
    if (sample) {
        std::string err;
        m.rings = measure_rings(w, o.gap_ms, o.samples, &err);
        m.rings_ok = !m.rings.moving() && m.rings.bytes_sampled > 0;
        if (!err.empty()) m.window_err = err;
    }
    m.mark = w.rd32(kWinMark) == kMarkMagic;
    m.mark2 = w.has(kWinMark2, 4) && w.rd32(kWinMark2) == kMarkMagic;
    m.data_words = w.has(kWinData, kWinDataLen) ? count_nonzero(w, kWinData, kWinDataLen) : 0;
    return m;
}

void print_mode(const ModeReport& m, bool sampled) {
    if (!m.window_ok) {
        log_note("window: not available (%s)", m.window_err.c_str());
    } else {
        log_note("window: base %#x", m.window_base);
        if (sampled) {
            std::string per;
            for (const auto& p : m.rings.per_ring)
                per += p.first + "=" + std::to_string(p.second) + " ";
            log_note("rings: %llu changed words (%s)-> %s",
                     static_cast<unsigned long long>(m.rings.changed_words), trim(per).c_str(),
                     m.rings_ok ? "quiet" : "MOVING");
        }
        log_note("mark  : %s; second marker: %s", m.mark ? "present" : "absent",
                 m.mark2 ? "present" : "absent");
        log_note("data  : %llu/%u non-zero words", static_cast<unsigned long long>(m.data_words),
                 kWinDataLen / 4);
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

std::string state_brief(const State& s) {
    if (!s.loaded) return "no persisted state";
    std::string r = s.state;
    if (!s.pending.empty()) r += " (pending=" + s.pending + ")";
    if (!s.updated.empty()) r += " " + s.updated;
    return r;
}

int action_check(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    std::printf("%s --check\n", kToolVersion);
    log_note("persisted state: %s", state_path.c_str());
    State st;
    std::string err;
    if (state_load(state_path, &st, &err))
        log_note("  %s", state_brief(st).c_str());
    else
        log_note("  could not read (%s)", err.c_str());

    RadiosReport r = radios_report();
    bool radios_ok = print_radio_table(r);
    log_note("wifi: %s; bluetooth: %s", r.wifi_ok ? "blocked" : "ON",
             r.bt_ok ? "blocked" : "ON");

    ModeReport m = inspect_mode(o, true);
    print_mode(m, true);

    bool mode_active = m.window_ok && m.rings_ok;
    if (!m.window_ok)
        log_note("mode  : undetermined (no window)");
    else if (mode_active && m.mark)
        log_note("mode  : SDR active and dump engine programmed");
    else if (mode_active)
        log_note("mode  : SDR active, no dump engine programmed");
    else
        log_note("mode  : modem in service (SDR mode not active)");

    if (!o.partition.empty() || !o.image_sha256_expected.empty()) {
        std::string part;
        if (resolve_partition(o, &part, &err)) {
            std::string hex;
            uint64_t size = 0;
            const uint64_t limit = expected_image_size(o, st);
            if (sha256_of_file(part, limit, &hex, &size, &err)) {
                log_note("partition: %s (%s%s)", part.c_str(), human_bytes(size).c_str(),
                         limit ? " usable" : " whole");
                log_note("sha256   : %s", hex.c_str());
                if (limit == 0)
                    log_note("no --size and no state: the whole partition is hashed, "
                             "it may not match the image");
                if (!o.image_sha256_expected.empty()) {
                    bool same = hex == o.image_sha256_expected;
                    log_note("expected : %s -> %s", o.image_sha256_expected.c_str(),
                             same ? "matches" : "DOES NOT MATCH");
                }
            } else {
                log_note("partition: %s not readable (%s)", part.c_str(), err.c_str());
            }
        } else {
            log_note("partition: %s", err.c_str());
        }
    }

    bool ready = radios_ok && r.ccmni_up.empty();
    std::printf("verdict: %s\n",
                ready ? (mode_active ? "SDR mode already active" : "ready to load (radios silenced)")
                      : "precondition NOT met");
    return ready ? RC_OK : RC_PRECONDITION;
}

int persist_or_fail(const std::string& path, const State& st, const char* what) {
    std::string err;
    if (!state_save(path, st, &err)) {
        log_error("could not save the state (%s) before %s: %s", path.c_str(), what,
                  err.c_str());
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Resuming: the flash is only written once
//
// A flash ends with pending=reboot-to-enter. After the reboot the activation has to be
// completed, and it must not write the partition again: the image is checked against the
// sha256 recorded at flash time and only then does the sequence go on. A partition that
// does not return that image stops here: nothing is activated on an unverified image.
// ---------------------------------------------------------------------------

struct ImageCheck {
    bool ok = false;
    bool readable = false;  // the partition could be read at all
    std::string sha;
    std::string why;
};

ImageCheck check_partition_image(const Options& o, const State& st) {
    ImageCheck c;
    if (st.image_sha256.empty() || st.image_size == 0) {
        c.why = "the state does not record the image sha256 and size";
        return c;
    }
    std::string part;
    if (!resolve_partition(o, &part, &c.why)) return c;
    std::string hex, err;
    uint64_t size = 0;
    if (!sha256_of_file(part, st.image_size, &hex, &size, &err)) {
        c.why = "cannot read " + part + ": " + err;
        return c;
    }
    c.readable = true;
    c.sha = hex;
    if (size != st.image_size) {
        c.why = "the partition returns " + std::to_string(size) + " usable bytes and the state " +
                "recorded " + std::to_string(st.image_size);
        return c;
    }
    if (hex != st.image_sha256) {
        c.why = "the partition holds " + hex + " and the state recorded " + st.image_sha256;
        return c;
    }
    c.ok = true;
    return c;
}

int resume_pending_flash(const Options& o, const std::string& state_path, State* st) {
    if (st->pending != "reboot-to-enter") return RC_OK;
    if (!reboot_happened(*st)) {
        log_error("a flash is pending reboot (state %s): reboot before activating", st->state.c_str());
        log_note("after the reboot: sudo sdr-load --resume (or --activate); neither writes the image again");
        return RC_REBOOT_NEEDED;
    }
    const ImageCheck chk = check_partition_image(o, *st);
    if (!chk.ok) {
        log_error("cannot confirm that the partition holds the image that was flashed: %s", chk.why.c_str());
        if (!chk.readable) {
            log_note("run it with privileges: sudo sdr-load --activate");
            return RC_PRECONDITION;
        }
        log_note("nothing is activated on an image that has not been verified");
        log_note("flash it again: sdr-load --image %s", st->image.empty() ? "<path>" : st->image.c_str());
        return RC_SHA_MISMATCH;
    }
    log_note("reboot detected and the partition holds the recorded image (%s)", chk.sha.c_str());
    log_note("the image is NOT written again: the flash is closed and the activation goes on");
    st->pending.clear();
    st->state = "ok";
    st->last_error.clear();
    st->boot_id = current_boot_id();
    if (persist_or_fail(state_path, *st, "closing the flash after the reboot")) return RC_USAGE;
    return RC_OK;
}

// ENTER_DEEP_FLIGHT does not stop the modem instantly: it first writes about two seconds of
// shutdown log and the link rings reach zero around six seconds after the ioctl (measured).
// A single look after --settle-ms catches that log still moving and calls a working mode
// "not entered". Poll until the rings are quiet, with a deadline.
bool wait_quiet_rings(const Options& o, int timeout_ms, ModeReport* last, int* waited_ms) {
    int waited = 0;
    if (o.settle_ms > 0) {
        ::usleep(static_cast<useconds_t>(o.settle_ms) * 1000);
        waited = o.settle_ms;
    }
    for (;;) {
        *last = inspect_mode(o, true);
        if (last->window_ok && last->rings_ok) {
            if (waited_ms) *waited_ms = waited;
            return true;
        }
        if (!last->window_ok || waited >= timeout_ms) {
            if (waited_ms) *waited_ms = waited;
            return false;
        }
        const int step = std::min(1000, timeout_ms - waited);
        ::usleep(static_cast<useconds_t>(step) * 1000);
        waited += step;
    }
}

// Steps 1 to 3 of the load sequence, shared by --no-flash and by an activation that finds
// the modem out of SDR mode (the usual case right after the reboot that follows a flash):
// block the radios, check that no data interface is left up, enter SDR mode and require
// the link rings to be quiet. If the mode is already entered it does nothing.
int enter_sdr_phase(const Options& o, const std::string& state_path, State* st) {
    const ModeReport before = inspect_mode(o, true);
    // Two independent readings are required before skipping the ioctl: the rings are quiet AND the
    // state says the mode was entered in this boot. Either one alone can be stale or
    // misleading, and requesting modes outside SDR mode is not acceptable.
    const bool entered = before.window_ok && before.rings_ok && st->pending.empty() &&
                         (st->state == "sdr-active" || st->state == "activating" ||
                          st->state == "activate-failed");
    if (entered) {
        log_note("SDR mode is already entered (rings quiet, state %s): nothing to do here",
                 st->state.c_str());
        return RC_OK;
    }
    if (::access(kIoctlNode, R_OK | W_OK) != 0) {
        log_error("cannot open %s: %s", kIoctlNode, std::strerror(errno));
        return RC_PRECONDITION;
    }

    log_note("radios: block WiFi and Bluetooth");
    st->state = "entering";
    int rc = block_radios(st, false);
    if (rc != 0) return rc;
    if (persist_or_fail(state_path, *st, "entering SDR mode")) return RC_USAGE;

    RadiosReport r = radios_report();
    print_radio_table(r);
    if (!r.ccmni_up.empty()) {
        log_error("ccmni* interface(s) are up; cut the data connection before entering "
                  "(nmcli radio wwan off or nmcli con down <id>)");
        return RC_PRECONDITION;
    }
    if (!r.wlan_up.empty()) {
        log_error("%zu wlan* interface(s) are up", r.wlan_up.size());
        return RC_PRECONDITION;
    }

    log_note("enter SDR mode (ioctl 0x430E)");
    int ioctl_errno = 0;
    if (enter_sdr_mode(&ioctl_errno) != 0) {
        log_error("enter ioctl failed: %s", std::strerror(ioctl_errno));
        st->state = "ok";
        st->last_error = std::string("ioctl 0x430E: ") + std::strerror(ioctl_errno);
        persist_or_fail(state_path, *st, "recording the failure");
        return RC_IOCTL_FAILED;
    }
    st->state = "sdr-active";
    st->last_error.clear();
    if (persist_or_fail(state_path, *st, "entering SDR mode")) return RC_USAGE;
    log_note("SDR mode active");

    log_note("waiting for the rings to go quiet (up to %d ms)", o.quiet_timeout_ms);
    ModeReport m;
    int waited = 0;
    const bool quiet = wait_quiet_rings(o, o.quiet_timeout_ms, &m, &waited);
    print_mode(m, true);
    if (!m.window_ok) {
        log_error("cannot verify the mode: %s", m.window_err.c_str());
        log_note("SDR mode may be active; check by hand and use --restore to go back");
        return RC_MODE_UNVERIFIED;
    }
    if (!quiet) {
        log_error("the rings are still moving after %d ms: the modem is not in SDR mode", waited);
        st->state = "entering";
        st->last_error = "rings still moving after " + std::to_string(waited) + " ms";
        persist_or_fail(state_path, *st, "recording the failure");
        log_note("use --restore to leave it as it was");
        return RC_MODE_UNVERIFIED;
    }
    log_note("SDR mode entered and verified (rings quiet after %d ms)", waited);
    return RC_OK;
}

int action_load(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    std::printf("%s --no-flash\n", kToolVersion);
    int step = 0;
    const int steps = 7;

    if (!o.dry_run && !running_as_root()) {
        log_error("root is required (ioctl on %s and read of %s)", kIoctlNode, kWindowNode);
        return RC_PRECONDITION;
    }
    if (!o.dry_run && ::access(kIoctlNode, R_OK | W_OK) != 0) {
        log_error("cannot open %s: %s", kIoctlNode, std::strerror(errno));
        return RC_PRECONDITION;
    }

    State st;
    std::string err;
    st.loaded = state_load(state_path, &st, &err);
    if (!st.loaded) {
        st = State();
        st.partition.clear();
    }
    if (st.loaded && st.pending == "reboot-to-enter") {
        if (reboot_happened(st)) {
            log_note("reboot detected since the flash: clearing the pending flag");
            st.pending.clear();
            st.state = "ok";
        } else {
            log_error("a flash is pending reboot (state %s); reboot before entering",
                      st.state.c_str());
            return RC_REBOOT_NEEDED;
        }
    }

    // Which image is in the partition? Entering SDR mode on the stock image leaves the
    // modem with no capture path, and the state is not asked: the partition is hashed.
    const PartitionFacts pf = partition_facts(o, st);
    const ImageTruth truth = image_truth(st, pf);
    if (pf.readable) {
        log_note("partition: %s, %s", short_sha(pf.sha256).c_str(), image_kind_text(pf).c_str());
        if (pf.stock)
            log_warn("the partition holds the stock image: entering SDR mode on it leaves the"
                     " modem without a capture path");
        if (truth.verdict == ImageVerdict::Incoherent)
            log_warn("the state says %s, but the partition holds %s: the state is not trusted",
                     st.state.c_str(), image_kind_text(pf).c_str());
    } else {
        log_warn("the image in the partition cannot be read (%s)", pf.err.c_str());
    }

    // A sweep image keeps the modem in service on purpose: entering SDR mode (deep flight)
    // stops it and the scan could not run. --activate is the only path that makes sense.
    const ImageProfile* ready_prof =
        pf.profile.empty() ? nullptr : find_profile_by_name(pf.profile);
    if (ready_prof && (ready_prof->scan || ready_prof->passive)) {
        log_error("this image is %s: the modem must stay in service",
                  ready_prof->scan ? "a sweep image (scan = 1)" : "a passive image");
        log_note("entering the mode would stop the modem and leave nothing to capture");
        log_note("use: sdr-load --activate");
        return RC_NO_PROFILE;
    }

    // Entering SDR mode on an image that has no mode profile leaves the modem stopped, with
    // no capture path. Waiting the full mark window for a dump that the image cannot produce
    // only delays the honest answer: say it now and point at --image.
    if (pf.readable && o.mode_profile.empty() &&
        ((!pf.whole && pf.profile.empty()) || pf.stock)) {
        log_error("the image has no mode profile: the partition holds %s",
                  image_kind_text(pf).c_str());
        log_note("entering the mode would leave the modem with nothing to capture: write the "
                 "mode image with sdr-load --image <path>");
        log_note("a sweep image is started with sdr-load --activate; to leave the mode: --restore");
        return RC_NO_PROFILE;
    }

    log_step(++step, steps, "radios: block WiFi and Bluetooth");
    st.state = "entering";
    st.pending.clear();
    int rc = block_radios(&st, o.dry_run);
    if (rc != 0) return rc;
    // Recorded now with the flags of what we blocked, so it can be reverted.
    if (!o.dry_run && persist_or_fail(state_path, st, "entering SDR mode")) return RC_USAGE;

    log_step(++step, steps, "data interfaces: check that none is left up");
    RadiosReport r = radios_report();
    print_radio_table(r);
    if (!r.ccmni_up.empty()) {
        const char* msg = "ccmni* interface(s) are up; cut the data connection before entering "
                          "(nmcli radio wwan off or nmcli con down <id>)";
        if (o.dry_run) log_warn("%s", msg);
        else {
            log_error("%s", msg);
            return RC_PRECONDITION;
        }
    }
    if (!r.wlan_up.empty()) {
        if (o.dry_run) log_warn("wlan* interface(s) are up (step 1 would take them down)");
        else {
            log_error("%zu wlan* interface(s) are up", r.wlan_up.size());
            return RC_PRECONDITION;
        }
    }

    if (!o.no_flash_sha256.empty()) {
        log_step(++step, steps, "image in the partition: verify sha256");
        std::string part;
        if (!resolve_partition(o, &part, &err)) {
            log_error("%s", err.c_str());
            return RC_SHA_MISMATCH;
        }
        std::string hex;
        uint64_t size = 0;
        bool from_state = false;
        const uint64_t limit = expected_image_size(o, st, &from_state);
        if (limit == 0) {
            log_error("the number of partition bytes that belong to the image is unknown");
            log_note("pass --size <bytes> (the image ones) or flash first with --image");
            return RC_USAGE;
        }
        if (!sha256_of_file(part, limit, &hex, &size, &err)) {
            log_error("could not read %s: %s", part.c_str(), err.c_str());
            return RC_SHA_MISMATCH;
        }
        log_note("partition %s (%s usable%s)", part.c_str(), human_bytes(size).c_str(),
                 from_state ? ", size from state" : "");
        log_note("sha256 %s", hex.c_str());
        if (hex != o.no_flash_sha256) {
            log_error("the partition does not hold the expected image (expected %s)",
                      o.no_flash_sha256.c_str());
            return RC_SHA_MISMATCH;
        }
    } else {
        log_step(++step, steps, "image in the partition: no --sha256, not verified");
    }

    log_step(++step, steps, "enter SDR mode (ioctl 0x430E)");
    if (o.dry_run) {
        log_note("(dry-run) the ioctl is not run");
    } else {
        int ioctl_errno = 0;
        if (enter_sdr_mode(&ioctl_errno) != 0) {
            log_error("enter ioctl failed: %s", std::strerror(ioctl_errno));
            st.state = "ok";
            st.last_error = std::string("ioctl 0x430E: ") + std::strerror(ioctl_errno);
            persist_or_fail(state_path, st, "recording the failure");
            return RC_IOCTL_FAILED;
        }
        st.state = "sdr-active";
        st.last_error.clear();
        if (persist_or_fail(state_path, st, "entering SDR mode")) return RC_USAGE;
        log_note("SDR mode active");
    }

    if (o.dry_run) {
        log_step(++step, steps, "load plan (dry-run)");
        for (const auto& s : plan_load(o, st)) log_note("(dry-run) %s", s.c_str());
        std::printf("result: dry-run: nothing has been touched\n");
        return RC_OK;
    }

    log_step(++step, steps, "verify quiet rings (up to %d ms, %d-sample windows)",
             o.quiet_timeout_ms, o.samples);
    ModeReport m;
    int waited = 0;
    const bool quiet = wait_quiet_rings(o, o.quiet_timeout_ms, &m, &waited);
    print_mode(m, true);

    int rc_final = RC_OK;
    if (!m.window_ok) {
        log_error("cannot verify the mode: %s", m.window_err.c_str());
        log_note("SDR mode may be active; check by hand and use --restore to go back");
        return RC_MODE_UNVERIFIED;
    }
    if (!quiet) {
        log_error("the rings are still moving after %d ms: the modem is not in SDR mode", waited);
        st.state = "entering";
        st.last_error = "rings still moving after " + std::to_string(waited) + " ms";
        persist_or_fail(state_path, st, "recording the failure");
        log_note("use --restore to leave it as it was");
        return RC_MODE_UNVERIFIED;
    }
    log_note("rings quiet after %d ms", waited);
    if (!m.mark) {
        // A sweep image captures through the firmware scanner and never programs the
        // dump engine: its mark is not expected, and waiting for it would be a 45 s
        // dead end ending in a code 12 that means nothing.
        const ImageProfile* prof =
            pf.profile.empty() ? nullptr : find_profile_by_name(pf.profile);
        if (prof && (prof->scan || prof->passive)) {
            log_note("the image is a sweep image (%s): the capture is produced by the scanner,"
                     " not by the dump engine; the mark is not expected", prof->name.c_str());
            log_note("start the reception with: sdr-load --activate");
        } else {
            // A single look can fall between bursts: the mark is looked for during a
            // window before it is called missing.
            log_step(++step, steps,
                     "dump: look for the engine mark (max %d s; it is written in bursts)",
                     o.mark_timeout_ms / 1000);
            const MarkProbe mp = probe_dump_mark(o.mark_timeout_ms);
            if (!mp.window) {
                log_error("cannot open %s to look for the mark: %s", kWindowNode, mp.err.c_str());
                log_note("the mode is loaded but the dump could not be verified");
                rc_final = RC_PRECONDITION;
            } else if (mp.ok) {
                log_note("dump-engine mark present after %d ms", mp.waited_ms);
                m.mark = true;
            } else {
                const MarkDiagnosis d = diagnose_missing_mark(
                    pf, o.no_flash_sha256, pf.profile, o.mark_timeout_ms,
                    "is the sweep running? the engine only publishes while it captures");
                log_error("%s", d.text.c_str());
                log_note("the modem is in SDR mode; to go back: sdr-load --restore");
                rc_final = d.rc;
            }
        }
    } else if (m.data_words == 0) {
        log_warn("mark present but the window has no data (frequency not set)");
    }

    log_step(++step, steps, "summary");
    log_note("state: %s", state_brief(st).c_str());
    log_note("revert with: sdr-load --restore");
    std::printf("result: %s\n", rc_final == RC_OK ? "SDR mode loaded and verified"
                                                  : "SDR mode active, dump not programmed");
    return rc_final;
}

int flash_image(const Options& o, const std::string& image, const std::string& partition,
                std::string* image_sha, uint64_t* image_size, std::string* err) {
    struct stat ist {};
    if (::stat(image.c_str(), &ist) != 0) {
        if (err) *err = image + " does not exist: " + std::strerror(errno);
        return RC_FLASH_FAILED;
    }
    uint64_t size = 0;
    std::string hex;
    if (!sha256_of_file(image, 0, &hex, &size, err)) return RC_FLASH_FAILED;
    log_note("image  : %s (%s)", image.c_str(), human_bytes(size).c_str());
    log_note("sha256 : %s", hex.c_str());
    if (image_sha) *image_sha = hex;
    if (image_size) *image_size = size;
    if (!o.image_sha256_expected.empty() && o.image_sha256_expected != hex) {
        if (err) *err = "the image sha256 does not match --sha256";
        return RC_SHA_MISMATCH;
    }
    const std::string script = flash_script_path(o);
    if (o.dry_run) {
        log_note("(dry-run) would run %s %s", script.c_str(), image.c_str());
        return RC_OK;
    }
    if (::access(script.c_str(), X_OK) != 0) {
        if (err) *err = "the flash script is not executable: " + script +
                        " (copy tools/flash_modem.sh to /home/furios/ or pass --flash-script)";
        return RC_FLASH_FAILED;
    }
    log_note("running %s %s", script.c_str(), image.c_str());
    int rc = run_inherit({script, image});
    if (rc != 0) {
        if (err) *err = "the flash script failed (code " + std::to_string(rc) + ")";
        return RC_FLASH_FAILED;
    }
    std::string got;
    uint64_t got_size = 0;
    if (!sha256_of_file(partition, size, &got, &got_size, err)) return RC_SHA_MISMATCH;
    log_note("partition readback: %s (%s)", got.c_str(), human_bytes(got_size).c_str());
    if (got != hex) {
        if (err) *err = "the partition does not return the image sha256";
        return RC_SHA_MISMATCH;
    }
    return RC_OK;
}

int action_flash(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    std::printf("%s --image %s\n", kToolVersion, o.image.c_str());
    if (!o.dry_run && !running_as_root()) {
        log_error("root is required to write the firmware partition");
        return RC_PRECONDITION;
    }
    struct stat ist {};
    if (::stat(o.image.c_str(), &ist) != 0) {
        log_error("image %s does not exist: %s", o.image.c_str(), std::strerror(errno));
        return RC_FLASH_FAILED;
    }
    std::string partition, err;
    if (!resolve_partition(o, &partition, &err)) {
        log_error("%s", err.c_str());
        return RC_USAGE;
    }
    if (::access(partition.c_str(), F_OK) != 0) {
        log_error("partition %s does not exist: %s", partition.c_str(), std::strerror(errno));
        return RC_PRECONDITION;
    }

    log_step(1, 6, "radios: block WiFi and Bluetooth before touching the firmware");
    State st;
    st.loaded = state_load(state_path, &st, &err);
    st.partition = partition;
    st.flash_script = o.flash_script;
    st.image = o.image;
    st.state = "flashing";
    st.pending.clear();
    int rc = block_radios(&st, o.dry_run);
    if (rc != 0) return rc;

    log_step(2, 6, "confirm and save state BEFORE writing");
    if (!o.dry_run && !confirm(o, "the firmware partition is about to be written; the new image is used on the next boot; continue?")) {
        log_error("cancelled by the user");
        return RC_USAGE;
    }
    if (!o.dry_run && persist_or_fail(state_path, st, "flashing")) return RC_USAGE;

    log_step(3, 6, "flash the image");
    std::string image_sha;
    uint64_t image_size = 0;
    rc = flash_image(o, o.image, partition, &image_sha, &image_size, &err);
    if (rc != RC_OK) {
        log_error("%s", err.c_str());
        st.state = "flash-failed";
        st.last_error = err;
        if (!o.dry_run) persist_or_fail(state_path, st, "recording the failure");
        return rc;
    }

    log_step(4, 6, "verify the partition sha256");
    log_note("%s", o.dry_run ? "(dry-run) skipped" : "matches the image");
    st.image_sha256 = image_sha;
    st.image_size = image_size;
    // The profile recorded by the state belongs to the image in the partition. A stock
    // image has none: leaving the old name behind would make the state repeat it after
    // the image is gone.
    const ImageProfile* flashed = find_profile_by_sha256(image_sha);
    st.mode_profile = flashed ? flashed->name : std::string();
    st.state = "ok";
    st.pending = "reboot-to-enter";
    st.last_error.clear();
    if (!o.dry_run && persist_or_fail(state_path, st, "confirming the flash")) return RC_USAGE;

    log_step(5, 6, "reboot");
    if (o.reboot && !o.dry_run) {
        if (!confirm(o, "reboot the phone now?")) {
            log_warn("reboot cancelled; the image will be applied on the next boot");
            log_note("after the reboot: sdr-load --resume (completes the activation, no new write)");
            return RC_REBOOT_NEEDED;
        }
        log_note("rebooting (systemctl reboot)");
        run_inherit({"systemctl", "reboot"});
        return RC_REBOOTING;
    }
    log_note("the loader only reads the partition at boot: a reboot is needed");
    log_note("reboot with: sudo systemctl reboot");
    log_note("after the reboot: sudo sdr-load --resume   (or: --no-flash --sha256 %s)", image_sha.c_str());

    log_step(6, 6, "summary");
    log_note("state: %s", state_brief(st).c_str());
    std::printf("result: %s\n",
                o.dry_run ? "dry-run: nothing has been written"
                          : "image flashed and verified; reboot so the modem uses it");
    return RC_REBOOT_NEEDED;
}

struct ModemCheck {
    bool observable = false;  // there is a window or busctl: an opinion is possible
    bool alive = false;
    bool ofono = false;
    bool at = false;
    bool rings_moving = false;
    std::string detail;
};

ModemCheck check_modem(const Options& o, int wait_ms) {
    ModemCheck c;
    const int step_ms = 500;
    int waited = 0;
    ModeReport m;
    do {
        m = inspect_mode(o, true);
        c.rings_moving = m.window_ok && !m.rings_ok;
        if (c.rings_moving || !m.window_ok) break;  // with no window there is no point waiting
        waited += step_ms;
        if (waited >= wait_ms) break;
        ::usleep(step_ms * 1000);
    } while (true);
    c.ofono = ofono_modem_present(&c.detail);
    c.at = at_responds(&c.detail);
    // Alive if it moves its rings (the modem stack is running) or answers AT.
    c.alive = c.rings_moving || c.at;
    c.observable = m.window_ok || command_exists("busctl");
    return c;
}

void print_modem(const ModemCheck& c) {
    if (!c.observable) log_note("no window and no busctl: the modem state cannot be observed");
    log_note("rings moving          : %s", c.rings_moving ? "yes" : "no");
    log_note("ril_0 object in ofono : %s", c.ofono ? "present" : "ABSENT");
    log_note("AT response           : %s", c.at ? "OK" : "no response");
}

int action_restore(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    std::printf("%s --restore\n", kToolVersion);
    int step = 0;
    const int steps = 6;

    if (!o.dry_run && !running_as_root()) {
        log_error("root is required (ioctl, rfkill and flashing)");
        return RC_PRECONDITION;
    }

    State st;
    std::string err;
    st.loaded = state_load(state_path, &st, &err);
    if (!st.loaded) {
        log_warn("no persisted state (%s); reverting with the defaults", err.c_str());
    } else {
        log_note("previous state: %s", state_brief(st).c_str());
    }

    log_step(++step, steps, "save state restoring BEFORE leaving SDR mode");
    st.state = "restoring";
    st.pending = "exit-mode";
    st.last_error.clear();
    if (!o.dry_run && persist_or_fail(state_path, st, "reverting")) return RC_USAGE;

    log_step(++step, steps, "leave SDR mode (ioctl 0x430F)");
    if (o.dry_run) {
        log_note("(dry-run) the ioctl is not run");
    } else {
        int ioctl_errno = 0;
        if (exit_sdr_mode(&ioctl_errno) != 0) {
            log_warn("exit ioctl failed: %s (the revert continues)",
                     std::strerror(ioctl_errno));
        } else {
            log_note("SDR mode disabled");
        }
    }

    log_step(++step, steps, "radios: unblock what we blocked");
    st.pending = "unblock-radios";
    if (!o.dry_run && persist_or_fail(state_path, st, "unblocking radios")) return RC_USAGE;
    if (unblock_radios(st, o.dry_run) != 0) log_warn("some unlock could not be applied");

    if (o.dry_run) {
        log_step(++step, steps, "recovery plan (dry-run)");
        for (const auto& s : plan_restore(o, st)) log_note("(dry-run) %s", s.c_str());
        return RC_OK;
    }

    log_step(++step, steps, "modem: restart ofono and wait for it to come back (max %d ms)", o.wait_ms);
    if (!o.dry_run) {
        if (command_exists("systemctl")) {
            int rc = run_inherit({"systemctl", "restart", "ofono"});
            if (rc != 0) log_warn("systemctl restart ofono returned %d", rc);
        }
        ::usleep(1000 * 1000);
    }
    ModemCheck c = check_modem(o, o.wait_ms);
    print_modem(c);

    if (!c.observable) {
        log_error("cannot check whether the modem came back: the firmware is not touched");
        log_note("the state is left at restoring; repeat --restore on the phone");
        return RC_RESTORE_INCOMPLETE;
    }

    if (c.alive) {
        st.state = "ok";
        st.pending.clear();
        st.wifi_blocked_by_tool = false;
        st.bt_blocked_by_tool = false;
        st.last_error.clear();
        if (persist_or_fail(state_path, st, "closing the revert")) return RC_USAGE;
        log_step(++step, steps, "summary");
        log_note("if the bearer does not come back: nmcli con up \"<connection>\"");
        std::printf("result: modem back; revert complete\n");
        return RC_OK;
    }

    log_step(++step, steps, "the modem does not come back: recover with a good image");
    st.state = "recovering";
    st.pending = "flash-recovery";
    st.last_error = "the modem does not answer after leaving SDR mode";
    if (persist_or_fail(state_path, st, "flashing the recovery image")) return RC_USAGE;
    log_note("recovery image: %s", o.recover_image.c_str());
    if (::access(o.recover_image.c_str(), R_OK) != 0) {
        log_error("cannot read %s: %s", o.recover_image.c_str(), std::strerror(errno));
        log_note("flash a good image by hand and reboot");
        st.state = "restore-failed";
        persist_or_fail(state_path, st, "recording the failure");
        return RC_RESTORE_INCOMPLETE;
    }
    std::string partition;
    if (!resolve_partition(o, &partition, &err)) {
        log_error("%s", err.c_str());
        st.state = "restore-failed";
        persist_or_fail(state_path, st, "recording the failure");
        return RC_RESTORE_INCOMPLETE;
    }
    st.partition = partition;
    st.image = o.recover_image;
    std::string recover_sha;
    uint64_t recover_size = 0;
    int rc = flash_image(o, o.recover_image, partition, &recover_sha, &recover_size, &err);
    if (rc != RC_OK) {
        log_error("the recovery failed: %s", err.c_str());
        st.state = "restore-failed";
        st.last_error = err;
        persist_or_fail(state_path, st, "recording the failure");
        return RC_RESTORE_INCOMPLETE;
    }
    st.image_sha256 = recover_sha;
    st.image_size = recover_size;
    st.mode_profile.clear();  // the recovery image is not a mode one
    log_note("recovery image flashed and verified");

    log_step(++step, steps, "last resort: check the modem and reboot");
    ModemCheck c2 = check_modem(o, 2000);
    print_modem(c2);
    st.state = "recovering";
    st.pending = "reboot-to-recover";
    persist_or_fail(state_path, st, "requesting a reboot");

    if (c2.alive) {
        st.state = "ok";
        st.pending.clear();
        persist_or_fail(state_path, st, "closing the revert");
        std::printf("result: modem back after reflashing\n");
        return RC_OK;
    }

    if (o.no_reboot) {
        log_note("reboot with: sudo systemctl reboot");
        log_note("when back: sdr-load --verify-boot");
        std::printf("result: good image flashed; reboot needed\n");
        return RC_RECOVERY_FLASHED;
    }
    if (!o.reboot && !confirm(o, "the modem does not answer; reboot the phone now?")) {
        log_warn("reboot not confirmed");
        log_note("reboot with: sudo systemctl reboot");
        log_note("when back: sdr-load --verify-boot");
        std::printf("result: good image flashed; reboot needed\n");
        return RC_RECOVERY_FLASHED;
    }
    log_warn("the modem does not answer: rebooting the phone as a last resort");
    log_note("when back: sdr-load --verify-boot");
    run_inherit({"systemctl", "reboot"});
    return RC_REBOOTING;
}

int action_status(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    std::printf("%s --status\n", kToolVersion);
    log_note("state file: %s", state_path.c_str());
    State st;
    std::string err;
    const StateRead sr = state_read(state_path, &st, &err);
    // The truth is computed before the state is printed: the state line must not claim
    // SDR mode without saying, in the same breath, whether the partition backs it.
    PartitionFacts pf = partition_facts(o, st);
    const ImageTruth truth = image_truth(st, pf);
    if (sr == StateRead::Ok) {
        if (!st.loaded_from.empty() && st.loaded_from != state_path)
            log_note("legacy    : %s (the canonical file does not exist; it is only read)",
                     st.loaded_from.c_str());
        std::string state_line = st.state;
        if (truth.verdict == ImageVerdict::Unverified && truth.state_claims_sdr)
            state_line += " (unverified: the partition could not be read)";
        log_note("state     : %s", state_line.c_str());
        log_note("pending   : %s", st.pending.empty() ? "-" : st.pending.c_str());
        log_note("updated   : %s", st.updated.c_str());
        if (!st.image.empty()) log_note("image     : %s", st.image.c_str());
        if (!st.image_sha256.empty()) log_note("sha256    : %s", st.image_sha256.c_str());
        if (st.image_size) log_note("bytes     : %llu", st.image_size);
        if (!st.partition.empty()) log_note("partition : %s", st.partition.c_str());
        log_note("radios    : wifi=%s bt=%s (blocked by the tool)",
                 st.wifi_blocked_by_tool ? "yes" : "no", st.bt_blocked_by_tool ? "yes" : "no");
        if (!st.mode_profile.empty()) log_note("profile   : %s", st.mode_profile.c_str());
        log_note("activation: loops=%s, rails=%s", st.loop_armed ? "armed" : "stopped",
                 st.rails_on ? "on" : "no record");
        if (!st.last_error.empty()) log_note("last error: %s", st.last_error.c_str());
    } else if (sr == StateRead::Missing) {
        log_note("state     : none (no state file at %s)", state_path.c_str());
    } else {
        // Nothing is reported from a file that could not be read: no defaults, no
        // "factory image". The state is simply unknown from here.
        log_note("state     : unreadable (%s)", err.c_str());
    }

    RadiosReport r = radios_report();
    print_radio_table(r);

    // The partition is the only truth about the image that is loaded. The state file is
    // a record of what the tool did; it goes stale the moment anything else writes the
    // partition. It is hashed once (above) and compared with both the state and the
    // table, so "sdr-active" can never be reported for a partition that holds the stock
    // image.
    log_note("partition : %s", pf.resolved ? pf.partition.c_str() : "unknown");
    if (pf.readable) {
        log_note("image now : %s", pf.sha256.c_str());
        log_note("image size: %llu B%s", static_cast<unsigned long long>(pf.size),
                 pf.whole ? " (whole partition: the image size is unknown)" : " usable");
        log_note("image kind: %s", image_kind_text(pf).c_str());
    } else {
        log_note("image now : not readable (%s)", pf.err.c_str());
    }
    log_note("image check: %s", truth.text.c_str());
    if (truth.verdict == ImageVerdict::Incoherent)
        log_warn("the state file describes an image that is not the one in the partition: "
                 "it is not trusted");

    ModeReport m = inspect_mode(o, true);
    print_mode(m, true);
    bool mode_active = m.window_ok && m.rings_ok;
    // A sweep image keeps the modem in service on purpose: its "mode" is the scanner
    // feeding the ring, so quiet rings mean "no scan", not "SDR mode not loaded".
    const ImageProfile* img_prof =
        pf.profile.empty() ? nullptr : find_profile_by_name(pf.profile);
    const bool sweep_image = img_prof && (img_prof->scan || img_prof->passive);
    const bool sweep_loaded = sweep_image && st.state == "sdr-active";
    log_note("observed  : %s",
             !m.window_ok       ? "undetermined"
             : sweep_loaded     ? "SDR active (sweep image; modem in service)"
             : mode_active      ? (m.mark ? "SDR active with dump" : "SDR active without dump")
                                : "modem in service");

    // SDR mode state: image/profile, PLL, rails, loops and ring heartbeat.
    log_note("SDR mode  :");
    ModeMap map;
    bool identified = false;
    std::string perr;
    ImageId mid;
    bool map_ok = resolve_mode_map(o, st, &map, &perr, &identified, &mid, &pf);
    if (mid.readable) {
        log_note("  image   : %s (%s%s)", mid.partition.c_str(), human_bytes(mid.size).c_str(),
                 mid.limit ? " usable" : ", whole partition");
        log_note("  sha256  : %s", mid.sha256.c_str());
    } else {
        log_note("  image   : %s", mid.err.c_str());
    }
    if (map_ok) {
        log_note("  profile : %s (%s%s)%s", map.name.c_str(), map.source.c_str(),
                 identified ? ", image verified by sha256" : "",
                 map.scan ? ", sweep image (scan = 1)"
                          : map.passive ? ", passive image" : "");
        g_profile_used = map.name;
    } else {
        log_note("  profile : UNKNOWN (%s)", perr.c_str());
        log_note("            --activate will refuse; use --profile <name|file>");
    }
    if (!m.window_ok) {
        log_note("  markers : not available (%s)", m.window_err.c_str());
    } else {
        Window w;
        std::string werr;
        if (!w.open(&werr)) {
            log_note("  markers : not available (%s)", werr.c_str());
        } else {
            PllInfo pll = read_pll(w);
            if (pll.valid) {
                log_note("  PLL     : channel %u, band %u, %u kHz", pll.channel, pll.band, pll.khz);
            } else {
                log_note("  PLL     : no copy at WIT+0x1B0 (not seeded)");
            }
            RailInfo rails = read_rails(w);
            if (rails.valid) {
                log_note("  rails   : VRF18=%s VFE28=%s VRFCK=%s", hex32(rails.con0[0]).c_str(),
                         hex32(rails.con0[1]).c_str(), hex32(rails.con0[2]).c_str());
                log_note("  keepalive: %s (PASS=%u PER=%u TCK=%u, writes %u/%u/%u)",
                         rails.armed == kMagicLoopRails ? "ARMED" : "stopped", rails.pass, rails.per,
                         rails.tck, rails.writes[0], rails.writes[1], rails.writes[2]);
            } else {
                log_note("  rails   : no rail loop markers (TAG %s)", hex32(rails.tag).c_str());
            }
            TimingLoopInfo tl = read_timing_loop(w);
            if (tl.valid) {
                log_note("  table   : %s (PASS=%u NEG=%u BAD=%u CDFFE=%d,%d BSIE=%d)",
                         tl.armed == kMagicLoopTiming ? "ARMED" : "stopped", tl.pass, tl.neg, tl.bad,
                         static_cast<int>(tl.last[0]), static_cast<int>(tl.last[1]),
                         static_cast<int>(tl.last[2]));
            } else {
                log_note("  table   : no timing loop markers (TAG %s)", hex32(tl.tag).c_str());
            }
            Beat b1 = read_beat(w);
            if (!b1.valid) {
                log_note("  ring    : no markers (WIT+0x90)");
            } else {
                ::usleep(static_cast<useconds_t>(o.beat_ms) * 1000);
                Beat b2 = read_beat(w);
                const bool beat = b2.seq != b1.seq || b2.cps != b1.cps || b2.blks != b1.blks;
                log_note("  ring    : seq %u -> %u, copies %u -> %u, blocks %u (%d ms) -> %s", b1.seq,
                         b2.seq, b1.cps, b2.cps, b2.blks, o.beat_ms, beat ? "LATE" : "QUIET");
            }
        }
    }

    std::string hint;
    if (sr == StateRead::Unreadable)
        hint = "the state file cannot be read: run it with privileges (sudo sdr-load --status)";
    else if (st.pending == "reboot-to-enter" && reboot_happened(st))
        hint = "you already rebooted: sdr-load --resume completes the activation without writing the image again";
    else if (st.pending == "reboot-to-enter") hint = "reboot and then: sdr-load --resume (or --activate)";
    else if (st.pending == "reboot-to-recover" && reboot_happened(st))
        hint = "you already rebooted: sdr-load --verify-boot";
    else if (st.pending == "reboot-to-recover") hint = "reboot and then: sdr-load --verify-boot";
    // Before any other advice: if the partition does not hold the image the state talks
    // about, the next step is to write it again. Starting the reception cannot work.
    else if (truth.verdict == ImageVerdict::Incoherent) hint = truth.hint;
    // Stock image (or any image with no profile): there is nothing to activate, the
    // partition has to be written with a mode image first.
    else if (pf.readable && pf.stock)
        hint = "the partition holds the stock image: load the SDR mode image with "
               "sdr-load --image <path>";
    else if (pf.readable && !pf.whole && pf.profile.empty())
        hint = "the partition image has no mode profile: load the SDR mode image with "
               "sdr-load --image <path>";
    else if (map_ok && map.scan)
        hint = "the partition holds the sweep image " + pf.profile +
               ": start it with sdr-load --activate";
    else if (sr == StateRead::Missing)
        hint = "no state file yet: if the modem does not come back, use --restore";
    else if (!m.window_ok) hint = "no window: run --status on the phone and as root";
    else if (st.state == "restoring" && mode_active) hint = "the modem is still in SDR mode: sdr-load --restore";
    else if (st.state == "recovering" && !mode_active) hint = "sdr-load --verify-boot to close it";
    else if (sweep_loaded)
        hint = mode_active
                   ? "sdr-load --activate launches the scan (no scan, no data)"
                   : "scan running: read the ring with the app; sdr-load --deactivate stops it";
    else if (st.state == "sdr-active" && !mode_active) hint = "sdr-load --verify-boot to close it";
    else if (st.state == "activate-failed") hint = "activation failed: check 'last error' and repeat with sdr-load --resume";
    else if (st.state == "activating") hint = "activation stopped halfway: sdr-load --resume, or --deactivate to stop the loops";
    else if (st.state == "sdr-active" && !st.loop_armed)
        hint = "SDR mode with the loops stopped: sdr-load --activate";
    else if (st.state == "sdr-active") hint = "to go back: sdr-load --restore";
    if (!hint.empty()) log_note("next      : %s", hint.c_str());
    return RC_OK;
}

int action_verify_boot(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    std::printf("%s --verify-boot\n", kToolVersion);
    State st;
    std::string err;
    if (!state_load(state_path, &st, &err)) {
        log_note("no persisted state (%s); nothing to close", err.c_str());
        return RC_OK;
    }
    log_note("previous state: %s", state_brief(st).c_str());
    if (!st.pending.empty() && !reboot_happened(st)) {
        log_error("the state asks for a reboot that has not happened yet (pending=%s)", st.pending.c_str());
        log_note("reboot with: sudo systemctl reboot");
        return RC_REBOOT_NEEDED;
    }
    ModemCheck c = check_modem(o, o.wait_ms);
    print_modem(c);
    if (!c.observable) {
        log_error("cannot check the modem from here (no window and no busctl)");
        log_note("run --verify-boot on the phone and as root");
        return RC_MODE_UNVERIFIED;
    }
    RadiosReport r = radios_report();
    log_note("radios: wifi=%s bt=%s", r.wifi_ok ? "blocked" : "ON",
             r.bt_ok ? "blocked" : "ON");

    if (st.pending == "reboot-to-enter") {
        if (!c.alive) {
            log_error("the modem does not answer after the reboot");
            log_note("try: sdr-load --restore --recover-image %s", o.recover_image.c_str());
            return RC_RESTORE_INCOMPLETE;
        }
        // The reboot is the moment to check that the partition really holds the image that
        // was flashed. Where it can be read, a different image is not accepted silently.
        if (!st.image_sha256.empty() && st.image_size != 0) {
            const ImageCheck chk = check_partition_image(o, st);
            if (chk.readable && !chk.ok) {
                log_error("the partition does not return the image that was flashed: %s", chk.why.c_str());
                log_note("flash it again before entering SDR mode; the pending flag is kept");
                st.last_error = chk.why;
                persist_or_fail(state_path, st, "recording the state");
                return RC_SHA_MISMATCH;
            }
            if (chk.ok) log_note("the partition holds the image that was flashed (%s)", chk.sha.c_str());
            else log_warn("the image in the partition cannot be checked here (%s)", chk.why.c_str());
        }
        st.state = "ok";
        st.pending.clear();
        st.last_error.clear();
        persist_or_fail(state_path, st, "closing the flash");
        log_note("new image running; next step: sdr-load --resume (enters SDR mode and activates,"
                 " without writing the image again)");
        std::printf("result: boot verified; SDR mode is not loaded yet\n");
        return RC_OK;
    }

    if (st.pending == "reboot-to-recover" || st.state == "recovering" ||
        st.state == "restoring" || st.state == "flashing") {
        if (!c.alive) {
            log_error("the modem is still not answering: the revert did not complete");
            log_note("try another image: sdr-load --restore --recover-image <path>");
            st.last_error = "no modem after the reboot";
            persist_or_fail(state_path, st, "recording the state");
            return RC_RESTORE_INCOMPLETE;
        }
        st.state = "ok";
        st.pending.clear();
        st.last_error.clear();
        persist_or_fail(state_path, st, "closing the revert");
        std::printf("result: modem back; revert complete\n");
        return RC_OK;
    }

    if (st.state == "sdr-active") {
        ModeReport m = inspect_mode(o, true);
        bool mode_active = m.window_ok && m.rings_ok;
        if (mode_active) {
            log_note("SDR mode is still active; to go back: sdr-load --restore");
            return RC_OK;
        }
        st.state = "ok";
        persist_or_fail(state_path, st, "closing the state");
        std::printf("result: the modem came back on its own; state closed\n");
        return RC_OK;
    }

    std::printf("result: nothing pending\n");
    return RC_OK;
}

// ---------------------------------------------------------------------------
// SDR mode activation: --activate and --deactivate
//
// The sequence is dictated by the image profile; every step is verified by reading
// markers (or the modem table) before the next one is taken as good:
//
//   1. image and profile (partition sha256 -> table; or --profile)
//   2. modem ready (md1.status = ready, with retries: it takes >20 s)
//   3. dump armed (profile poke mode) and read back
//   4. PLL seeded (pll_seed mode) -> pll_channel_data
//   5. rails one at a time (modes 27/28/29) -> CON0 |= 1, read back
//   6. rail keepalive (mode 30) -> X3_ARMED = KPL1, PASS >= 1
//   7. timing table (mode 32) -> X4_ARMED = KPL2 and CDFFE/BSIE
//   8. ring (seq/copies) and PLL and rails read back
//
// A rule that is never skipped, not even when undoing: CON0 is only written with
// |= 1, NEVER 0. That is why --deactivate stops the loops and leaves the rails as
// they are: powering them down is a job for the modem restart or the power supply,
// not for this program.
// ---------------------------------------------------------------------------

bool check_rail_mode(const Window& w, const ModeMap& map, int mode, uint32_t addr,
                     uint32_t* con0_before, uint32_t* con0_after, std::string* why) {
    uint32_t tag = 0, stg = 0, xmode = 0, xaddr = 0, anom = 0, v0 = 0, v1 = 0;
    if (!wit_read(w, kMarkX2Tag, &tag) || tag != kTagR150) {
        if (why) *why = "no rail mode markers (TAG R150)";
        return false;
    }
    wit_read(w, kMarkX2Stg, &stg);
    wit_read(w, kMarkX2Mode, &xmode);
    wit_read(w, kMarkX2Addr, &xaddr);
    wit_read(w, kMarkX2Anom, &anom);
    wit_read(w, kMarkX2V0, &v0);
    wit_read(w, kMarkX2V1, &v1);
    if (con0_before) *con0_before = v0;
    if (con0_after) *con0_after = v1;
    if (xmode != static_cast<uint32_t>(mode)) {
        if (why) *why = "the markers are not from this mode (MODE=" + std::to_string(xmode) + ")";
        return false;
    }
    if (stg == 0) {
        if (why) *why = "the cave left no step in the markers (STG=0)";
        return false;
    }
    if (xaddr != addr) {
        if (why) *why = "the mode touched another register (" + hex32(xaddr) + ", " + hex32(addr) + " was expected)";
        return false;
    }
    if (anom != 0) {
        if (why)
            *why = "the guard refused the write: CON0 was " + hex32(v0) +
                   " (it is only written if it holds 0 or 1)";
        return false;
    }
    if ((v1 & map.rail_bit) == 0) {
        if (why) *why = "CON0 stayed at " + hex32(v1) + ": the rail is not powered";
        return false;
    }
    return true;
}

int activate_fail(Window& w, const ModeMap& map, const std::string& state_path, State* st,
                  const std::string& why, int rc) {
    log_error("%s", why.c_str());
    log_warn("undoing: stop the loops (mode %d)", map.loop_stop);
    ModeResult r = request_mode(w, map, map.loop_stop, 15000);
    if (!r.ok) {
        log_error("could not stop the loop: %s", r.err.c_str());
        log_note("the loop may still be armed: repeat --deactivate or restart the modem");
    } else {
        uint32_t a3 = 0, a4 = 0;
        wit_read(w, kMarkX3Armed, &a3);
        wit_read(w, kMarkX4Armed, &a4);
        log_note("loops stopped: X3_ARMED=%s X4_ARMED=%s", hex32(a3).c_str(), hex32(a4).c_str());
    }
    RailInfo rails = read_rails(w);
    if (rails.valid) {
        log_note("rails: VRF18=%s VFE28=%s VRFCK=%s", hex32(rails.con0[0]).c_str(),
                 hex32(rails.con0[1]).c_str(), hex32(rails.con0[2]).c_str());
        log_warn("the rails are NOT powered down in software (CON0 is only ever written with |= 1)");
        st->rails_on = (rails.con0[0] & map.rail_bit) && (rails.con0[1] & map.rail_bit) &&
                       (rails.con0[2] & map.rail_bit);
    }
    st->state = "activate-failed";
    st->loop_armed = false;
    st->last_error = why;
    persist_or_fail(state_path, *st, "recording the activation failure");
    log_note("to go back to the normal phone: sdr-load --restore");
    return rc;
}

std::vector<std::string> plan_activate_scan(const Options& o, const ModeMap& m) {
    std::vector<std::string> p;
    p.push_back("identify the partition image by sha256 and pick the profile (" + m.name + ")");
    p.push_back("wait for vendor.mtk.md1.status = ready, re-reading every second until " +
                std::to_string(o.md1_timeout_ms) + " ms");
    p.push_back("open the window and read the ring counters (PUSH/CAPS)");
    p.push_back("launch AT+COPS=? in the background (the scan that triggers the capture)");
    p.push_back("require PUSH or CAPS to advance within 45 s (otherwise there is no capture to draw)");
    p.push_back("read page+12 and require band " + std::to_string(m.fixed_band) + " and " +
                std::to_string(m.fixed_khz) + " kHz");
    p.push_back("save state sdr-active (this image has no loops to arm)");
    return p;
}

// Activation of a sweep image with fixed tuning. It asks for no mode numbers: the
// capture is produced by the firmware scanner and the cave fixes band/frequency. Only
// what matters is checked: that the ring advances and that the page stays on the fixed
// frequency.
int activate_scan(const Options& o, const ModeMap& map, const std::string& state_path, State* st) {
    int step = 0;
    const int steps = 7;

    auto fail = [&](const std::string& why, int rc) {
        st->state = "activate-failed";
        st->loop_armed = false;
        st->last_error = why;
        persist_or_fail(state_path, *st, "recording the activation failure");
        log_error("%s", why.c_str());
        log_note("to go back to the normal phone: sdr-load --restore");
        return rc;
    };

    log_step(++step, steps, "modem: wait for md1.status = ready (max %d ms)", o.md1_timeout_ms);
    std::string md1, err;
    if (!wait_md1_ready(o.md1_timeout_ms, &md1, &err)) return fail(err, RC_PRECONDITION);
    log_note("md1.status = ready");

    log_step(++step, steps, "window: open and read the ring counters");
    Window w;
    if (!w.open_rw(&err)) return fail("cannot open " + std::string(kWindowNode) + ": " + err,
                                      RC_PRECONDITION);
    uint32_t push0 = 0, caps0 = 0, blks0 = 0;
    const bool hp = wit_read(w, kMarkPPush, &push0);
    const bool hc = wit_read(w, kMarkPCaps, &caps0);
    wit_read(w, kMarkBlks, &blks0);
    if (!hp && !hc) return fail("no ring markers (WIT+0x08/+0x0c)", RC_ACTIVATE_FAILED);
    log_note("ring before: push=%u caps=%u blocks=%u", push0, caps0, blks0);

    log_step(++step, steps, "scan: launch AT+COPS=? in the background");
    std::string at_out;
    // The ofono AT proxy takes a few seconds after boot; without this the command is
    // lost to a timeout even though the modem is already ready.
    if (!at_responds(&at_out, 1)) {
        for (int i = 0; i < 20; ++i) {
            ::usleep(1000 * 1000);
            if (at_responds(&at_out, 1)) break;
        }
    }
    const std::vector<std::string> cops = {"busctl", "--timeout=600", "call", "org.ofono",
                                           "/ril_0", "org.ofono.FuriLabs.AT", "SendCommand",
                                           "s", "\"AT+COPS=?\""};
    if (!run_detached(cops))
        log_warn("could not launch AT+COPS=?; the scan may not start");

    log_step(++step, steps, "ring: require PUSH or CAPS to advance (max 45 s)");
    uint32_t push = push0, caps = caps0;
    bool moved = false;
    for (int i = 0; i < 90; ++i) {
        ::usleep(500 * 1000);
        wit_read(w, kMarkPPush, &push);
        wit_read(w, kMarkPCaps, &caps);
        if (static_cast<std::int32_t>(push - push0) > 0 ||
            static_cast<std::int32_t>(caps - caps0) > 0) {
            moved = true;
            break;
        }
    }
    if (!moved)
        return fail("the ring did not advance in 45 s: the scan produced no captures"
                    " (try AT+CFUN=0/1 and again, or --restore)",
                    RC_ACTIVATE_FAILED);
    log_note("ring after: push=%u caps=%u (advanced)", push, caps);

    log_step(++step, steps, "frequency: read page+12 of the last capture");
    uint32_t p3 = 0;
    if (!wit_read(w, kMarkPage3, &p3))
        return fail("cannot read page+12 in the window", RC_ACTIVATE_FAILED);
    const unsigned band = p3 & 0xFFFFu;
    const unsigned khz = p3 >> 16;
    log_note("page+12 = %s  (band %u, %u kHz)", hex32(p3).c_str(), band, khz);
    if (map.fixed_khz != 0 && (khz != map.fixed_khz || band != static_cast<unsigned>(map.fixed_band)))
        return fail("the page is not on the fixed frequency: band " +
                        std::to_string(map.fixed_band) + " and " + std::to_string(map.fixed_khz) +
                        " kHz were expected, and it says band " + std::to_string(band) + " and " +
                        std::to_string(khz) + " kHz",
                    RC_ACTIVATE_FAILED);

    log_step(++step, steps, "summary");
    st->state = "sdr-active";
    st->loop_armed = false;  // this generation does not arm loops
    st->rails_on = false;
    st->last_error.clear();
    st->mode_profile = map.name;
    if (persist_or_fail(state_path, *st, "closing the activation")) return RC_USAGE;
    st->loaded = true;
    ::waitpid(-1, nullptr, WNOHANG);
    log_note("state: %s", state_brief(*st).c_str());
    log_note("the capture is produced by the scanner: no scan, no data");
    log_note("go all the way back: sdr-load --restore");
    std::printf("result: SDR mode activated (scan launched and page at %u kHz)\n", khz);
    return RC_OK;
}

int action_activate(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    if (!o.via_resume) std::printf("%s --activate\n", kToolVersion);
    int step = 0;
    const int steps = 9;

    State st;
    std::string err;
    const StateRead sr = state_read(state_path, &st, &err);
    if (sr == StateRead::Unreadable) {
        // Without the state there is no way to know what was blocked or flashed: it is
        // not replaced by a default one.
        log_error("the state file %s cannot be read: %s", state_path.c_str(), err.c_str());
        if (running_as_root())
            log_note("if it is corrupt, move it away and start from scratch;"
                     " nothing is touched here");
        else
            log_note("run it with privileges: sudo sdr-load %s",
                     o.via_resume ? "--resume" : "--activate");
        return RC_PRECONDITION;
    }
    if (sr == StateRead::Missing) st = State();

    // A flash or a revert that stopped halfway owns the phone until it is closed: asking
    // for modes now would work on an image and a modem the tool has not verified.
    if (st.pending == "reboot-to-recover" || st.state == "recovering" ||
        st.state == "restoring" || st.state == "flashing") {
        log_error("a %s is pending (state %s, pending %s): close it before activating",
                  st.state == "flashing" ? "flash" : "revert", st.state.c_str(),
                  st.pending.empty() ? "-" : st.pending.c_str());
        log_note("run: sudo sdr-load --resume (or --verify-boot)");
        return RC_PRECONDITION;
    }

    // 1. The partition, then the profile. The partition is hashed once here and the map
    // is resolved from those bytes: a state that says sdr-active cannot make the tool
    // work on an image that is not there.
    log_step(++step, steps, "image and mode profile (the partition first)");
    const PartitionFacts pf = partition_facts(o, st);
    const ImageTruth truth = image_truth(st, pf);
    const ImageProfile* named_profile =
        o.mode_profile.empty() ? nullptr : find_profile_by_name(o.mode_profile);
    if (pf.whole && named_profile && !named_profile->sha256.empty()) {
        log_error("--profile %s with an unknown partition size: the image cannot be verified",
                  named_profile->name.c_str());
        log_note("pass --size <bytes> (the image bytes) or flash it first with --image");
        return RC_USAGE;
    }
    ModeMap map;
    bool identified = false;
    if (pf.readable && !pf.whole && !o.image_sha256_expected.empty() &&
        pf.sha256 != o.image_sha256_expected) {
        log_error("the partition has sha256 %s and %s was expected",
                  pf.sha256.c_str(), o.image_sha256_expected.c_str());
        log_note("write the image you expect, or drop --sha256");
        return RC_SHA_MISMATCH;
    }
    if (!resolve_mode_map(o, st, &map, &err, &identified, nullptr, &pf)) {
        if (o.dry_run) {
            log_warn("cannot identify the image here (%s)", err.c_str());
            log_note("the plan is shown with the first known profile; on the phone it is chosen by sha256");
            const ImageProfile* ex = find_profile_by_name("standard-a");
            if (!ex) return RC_NO_PROFILE;
            map = map_from_profile(*ex);
        } else {
            log_error("%s", err.c_str());
            if (pf.readable && !pf.whole && pf.profile.empty()) {
                // Not a new image: the usual cause is that the mode image was written
                // back and the state did not notice. "Repeat the activation" would be
                // a dead end; the only way out is to write the image again.
                log_note("the image is not in place: the partition holds %s (%s)",
                         short_sha(pf.sha256).c_str(), image_kind_text(pf).c_str());
                if (pf.stock)
                    log_note("that is the stock image: it has no dump engine to activate");
                if (truth.verdict == ImageVerdict::Incoherent)
                    log_note("the state says %s, but the state is not the partition:"
                             " it is not trusted", st.state.c_str());
                log_note("write the mode image again: sudo sdr-load --image <path>"
                         " (reboot afterwards)");
                return RC_SHA_MISMATCH;
            }
            log_note("known profiles: %s", known_profile_names().c_str());
            log_note("if the image is new, write a profile and pass it with --profile <file>");
            log_note("a profile table is shown with: sdr-load --modes --profile <name|file>");
            return RC_NO_PROFILE;
        }
    }
    log_note("partition: %s, %s", short_sha(pf.sha256).c_str(), image_kind_text(pf).c_str());
    log_note("profile: %s (%s)%s", map.name.c_str(), map.source.c_str(),
             identified ? ", image verified by sha256" : "");
    g_profile_used = map.name;
    if (!map.note.empty()) log_note("  %s", map.note.c_str());
    if (truth.verdict == ImageVerdict::Incoherent)
        log_warn("the state recorded another image (%s): %s is what is activated",
                 short_sha(st.image_sha256).c_str(), short_sha(pf.sha256).c_str());
    else if (!pf.readable)
        log_warn("the partition could not be read (%s): the image is not verified",
                 pf.err.c_str());

    // A flash leaves pending=reboot-to-enter. If the reboot has already happened and the
    // partition still holds the image that was recorded, the activation continues from
    // here: the image is NOT written again.
    if (!o.dry_run) {
        const int rc_resume = resume_pending_flash(o, state_path, &st);
        if (rc_resume != RC_OK) return rc_resume;
    }

    // Sweep image with fixed tuning: it is activated by starting the firmware scan.
    if (map.scan) {
        if (o.dry_run) {
            log_step(++step, 7, "activation plan (dry-run)");
            for (const auto& s : plan_activate_scan(o, map)) log_note("(dry-run) %s", s.c_str());
            std::printf("result: dry-run: nothing has been touched\n");
            return RC_OK;
        }
        if (!running_as_root()) {
            log_error("root is required (modem command block at %s)", kWindowNode);
            return RC_PRECONDITION;
        }
        return activate_scan(o, map, state_path, &st);
    }

    // A passive image has no sequence: its modem copies the scanner capture without the
    // AP writing a single mode. It is refused before touching anything.
    if (map.passive) {
        log_error("image %s is not activated by modes: the modem copies the scanner capture"
                  " on its own", map.name.c_str());
        log_note("there is no engine to arm, no PLL to seed and no rails to power");
        log_note("load the image (--image) or take the one already there as good (--no-flash);"
                 " start the sweep and read the ring");
        return RC_NO_PROFILE;
    }

    if (o.dry_run) {
        log_step(++step, steps, "activation plan (dry-run)");
        if (st.pending == "reboot-to-enter")
            log_note("(dry-run) pending flash: the partition would be checked against %s"
                     " and NOT written", st.image_sha256.c_str());
        for (const auto& s : plan_activate(o, map)) log_note("(dry-run) %s", s.c_str());
        log_note("(dry-run) mode table of profile %s:", map.name.c_str());
        for (const auto& e : map.modes) log_note("  mode %2d  %s", e.mode, e.what.c_str());
        std::printf("result: dry-run: nothing has been touched\n");
        return RC_OK;
    }

    if (!running_as_root()) {
        log_error("root is required (modem command block at %s)", kWindowNode);
        return RC_PRECONDITION;
    }

    // Nothing is entered on the stock image, not even when a profile file without a
    // sha256 asserted it: that image has no dump engine and the phone would be left in
    // SDR mode with no way to capture.
    if (pf.readable && pf.stock) {
        log_error("the partition holds the stock image (%s): there is no dump engine to activate",
                  short_sha(pf.sha256).c_str());
        log_note("write the mode image first: sudo sdr-load --image <path> (reboot afterwards)");
        return RC_SHA_MISMATCH;
    }

    // The modes only exist in SDR mode. If the modem is not there yet (the usual case
    // after the reboot that follows a flash) it is entered here, as --no-flash does; if it
    // is already entered this does nothing. That is what makes --activate idempotent.
    const int rc_enter = enter_sdr_phase(o, state_path, &st);
    if (rc_enter != RC_OK) return rc_enter;

    // 2. Modem ready.
    log_step(++step, steps, "modem: wait for md1.status = ready (max %d ms)", o.md1_timeout_ms);
    std::string md1;
    if (!wait_md1_ready(o.md1_timeout_ms, &md1, &err)) {
        log_error("%s", err.c_str());
        log_note("if you just flashed, wait and repeat; if the modem does not come back: --verify-boot");
        return RC_PRECONDITION;
    }
    log_note("md1.status = ready");

    // 3. Read/write window and armed dump.
    log_step(++step, steps, "dump: arm the engine (mode %d)", map.poke_mode);
    Window w;
    if (!w.open_rw(&err)) {
        log_error("cannot open %s read/write: %s", kWindowNode, err.c_str());
        return RC_PRECONDITION;
    }
    uint32_t wit_off = 0;
    if (!w.va_to_off(kMarkVa, kMarkSpan, &wit_off)) {
        log_error("the window (base %s, %zu B) does not reach the modem markers (%s)",
                  hex32(w.base()).c_str(), w.length(), hex32(kMarkVa).c_str());
        return RC_PRECONDITION;
    }
    log_note("window: base %s, %zu B, markers at +%s", hex32(w.base()).c_str(), w.length(),
             hex32(wit_off).c_str());

    st.state = "activating";
    st.mode_profile = map.name;
    st.loop_armed = false;
    st.rails_on = false;
    st.last_error.clear();
    if (persist_or_fail(state_path, st, "activating SDR mode")) return RC_USAGE;

    if (map.arm_addr != 0) {
        std::vector<uint32_t> rb;
        const std::vector<uint32_t> addrs{map.arm_addr};
        const std::vector<uint32_t> values{map.arm_value};
        if (!modem_poke(w, map, addrs, values, &rb, &err))
            return activate_fail(w, map, state_path, &st, "could not arm the dump: " + err,
                                 RC_ACTIVATE_FAILED);
        log_note("%s <- %s (releido)", hex32(map.arm_addr).c_str(),
                 hex32(rb.empty() ? 0 : rb[0]).c_str());
        // The mark is the evidence that the engine was left programmed; it is written in
        // bursts, so it is looked for during a window and not once. If it never appears,
        // the message says whether the image is the right one or not: a reverted image
        // is not a modem that has not published yet.
        log_note("waiting for the dump-engine mark (max %d s; it is written in bursts)",
                 o.mark_timeout_ms / 1000);
        const MarkProbe mark = wait_dump_mark(w, o.mark_timeout_ms);
        if (!mark.window) {
            return activate_fail(w, map, state_path, &st,
                                 "the window is not open, so the dump mark cannot be looked for",
                                 RC_PRECONDITION);
        }
        if (!mark.ok) {
            const MarkDiagnosis d = diagnose_missing_mark(
                pf, map.sha256, map.name, o.mark_timeout_ms,
                "the modem did not leave the mark after arming the engine");
            return activate_fail(w, map, state_path, &st, d.text, d.rc);
        }
        log_note("dump-engine mark present after %d ms", mark.waited_ms);
    } else {
        log_warn("the profile declares no arming (arm_addr): the dump is assumed already armed");
    }

    // 4. PLL.
    std::string what;
    map.describe(map.pll_seed, &what);
    log_step(++step, steps, "PLL: seed (%s)", what.c_str());
    ModeResult r = request_mode(w, map, map.pll_seed, o.mode_timeout_ms);
    if (!r.ok)
        return activate_fail(w, map, state_path, &st, "could not seed the PLL: " + r.err,
                             RC_ACTIVATE_FAILED);
    PllInfo pll = read_pll(w);
    if (!pll_matches(pll, map, &err))
        return activate_fail(w, map, state_path, &st, err, RC_ACTIVATE_FAILED);
    log_note("pll_channel_data: channel %u, band %u, %u kHz (after %d ms)", pll.channel, pll.band,
             pll.khz, r.waited_ms);

    // 5. Rails, one at a time.
    log_step(++step, steps, "rails: CON0 |= 1 on VRF18, VFE28 and VRFCK (one at a time)");
    struct RailSpec {
        const char* name;
        int mode;
        uint32_t addr;
    };
    const RailSpec rail_specs[3] = {{"VRF18", map.rail_v18, 0x1BAC},
                                    {"VFE28", map.rail_v28, 0x1B88},
                                    {"VRFCK", map.rail_vck, 0x1D1C}};
    for (const auto& rs : rail_specs) {
        ModeResult rr = request_mode(w, map, rs.mode, o.mode_timeout_ms);
        if (!rr.ok)
            return activate_fail(w, map, state_path, &st,
                                 std::string("could not power ") + rs.name + ": " + rr.err,
                                 RC_ACTIVATE_FAILED);
        uint32_t v0 = 0, v1 = 0;
        std::string why;
        if (!check_rail_mode(w, map, rs.mode, rs.addr, &v0, &v1, &why))
            return activate_fail(w, map, state_path, &st, std::string(rs.name) + ": " + why,
                                 RC_ACTIVATE_FAILED);
        log_note("%s (CON0 %s): %s -> %s (|= 1)", rs.name, hex32(rs.addr).c_str(), hex32(v0).c_str(),
                 hex32(v1).c_str());
        const std::string md1b = md1_status();
        if (!md1b.empty() && md1b != "ready")
            return activate_fail(w, map, state_path, &st,
                                 "the modem stopped being ready while powering " + std::string(rs.name) +
                                     " (md1.status = " + md1b + ")",
                                 RC_ACTIVATE_FAILED);
    }

    // 6. Rail keepalive.
    map.describe(map.keepalive, &what);
    log_step(++step, steps, "loop: rail keepalive (%s)", what.c_str());
    if (map.loop_period != 0 && !wit_write(w, kMarkCfgPer, map.loop_period))
        return activate_fail(w, map, state_path, &st, "cannot write CFG_PER", RC_ACTIVATE_FAILED);
    ModeResult r30 = request_mode(w, map, map.keepalive, o.mode_timeout_ms);
    if (!r30.ok)
        return activate_fail(w, map, state_path, &st, "could not arm the keepalive: " + r30.err,
                             RC_ACTIVATE_FAILED);
    RailInfo rails1 = read_rails(w);
    if (!rails1.valid)
        return activate_fail(w, map, state_path, &st,
                             "no rail loop markers (TAG " + hex32(rails1.tag) + ")",
                             RC_ACTIVATE_FAILED);
    if (rails1.armed != kMagicLoopRails)
        return activate_fail(w, map, state_path, &st,
                             "the keepalive was not left armed (X3_ARMED = " + hex32(rails1.armed) + ")",
                             RC_ACTIVATE_FAILED);
    if (rails1.pass < 1)
        return activate_fail(w, map, state_path, &st,
                             "the keepalive did not run a single pass (X3_PASS = 0)", RC_ACTIVATE_FAILED);
    if (map.loop_period != 0 && rails1.per != map.loop_period)
        return activate_fail(w, map, state_path, &st,
                             "the loop period was left at " + std::to_string(rails1.per) +
                                 " (" + std::to_string(map.loop_period) + " was requested)",
                             RC_ACTIVATE_FAILED);
    log_note("keepalive armed: X3_ARMED=KPL1 PASS=%u PER=%u TCK=%u TN=%u", rails1.pass, rails1.per,
             rails1.tck, rails1.tn);

    // 7. Timing table.
    map.describe(map.timing, &what);
    log_step(++step, steps, "loop: timing table (%s)", what.c_str());
    ModeResult r32 = request_mode(w, map, map.timing, o.mode_timeout_ms);
    if (!r32.ok)
        return activate_fail(w, map, state_path, &st, "could not arm the table: " + r32.err,
                             RC_ACTIVATE_FAILED);
    TimingLoopInfo tl = read_timing_loop(w);
    if (!tl.valid)
        return activate_fail(w, map, state_path, &st,
                             "no timing loop markers (TAG " + hex32(tl.tag) + ")",
                             RC_ACTIVATE_FAILED);
    if (tl.armed != kMagicLoopTiming)
        return activate_fail(w, map, state_path, &st,
                             "the timing loop was not left armed (X4_ARMED = " + hex32(tl.armed) + ")",
                             RC_ACTIVATE_FAILED);
    if (tl.pass < 1)
        return activate_fail(w, map, state_path, &st,
                             "the timing loop did not run a single pass (X4_PASS = 0)",
                             RC_ACTIVATE_FAILED);

    const std::vector<uint32_t> tab_addrs{kTimingTableA, kTimingTableA + 4, kTimingTableB,
                                          kTimingTableB + 4, kTimingTableB + 8};
    std::vector<uint32_t> tab;
    bool tab_ok = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!modem_peek(w, map, tab_addrs, &tab, &err))
            return activate_fail(w, map, state_path, &st, "cannot read the table: " + err,
                                 RC_ACTIVATE_FAILED);
        tab_ok = tab.size() == 5 && tab[0] == map.cdf0 && tab[1] == map.cdf1 &&
                 tab[2] == map.bsi && tab[3] == map.bsi && tab[4] == map.bsi;
        if (tab_ok) break;
        log_warn("the table does not hold the expected values; the loop is re-armed once");
        ModeResult retry = request_mode(w, map, map.timing, o.mode_timeout_ms);
        if (!retry.ok)
            return activate_fail(w, map, state_path, &st,
                                 "the table could not be re-armed: " + retry.err, RC_ACTIVATE_FAILED);
    }
    if (!tab_ok)
        return activate_fail(w, map, state_path, &st,
                             "timing table A / timing table B do not hold " +
                                 std::to_string(map.cdf0) + "/" + std::to_string(map.cdf1) + "/" +
                                 std::to_string(map.bsi) + " (read " + hex32(tab.empty() ? 0 : tab[0]) +
                                 "," + hex32(tab.size() < 2 ? 0 : tab[1]) + "," +
                                 hex32(tab.size() < 3 ? 0 : tab[2]) + ")",
                             RC_ACTIVATE_FAILED);
    log_note("table: timing table A %u,%u  timing table B %u,%u,%u", tab[0], tab[1], tab[2],
             tab[3], tab[4]);

    // 8. Ring, PLL and rails.
    log_step(++step, steps, "verify: the ring beats and the PLL/rails are preserved");
    Beat b1 = read_beat(w);
    if (!b1.valid)
        return activate_fail(w, map, state_path, &st, "no ring markers (WIT+0x90)",
                             RC_ACTIVATE_FAILED);
    ::usleep(static_cast<useconds_t>(o.beat_ms) * 1000);
    Beat b2 = read_beat(w);
    if (!b2.valid)
        return activate_fail(w, map, state_path, &st, "no ring markers (WIT+0x90)",
                             RC_ACTIVATE_FAILED);
    if (b2.seq == b1.seq && b2.cps == b1.cps && b2.blks == b1.blks)
        return activate_fail(w, map, state_path, &st,
                             "the ring does not beat: seq " + std::to_string(b1.seq) + " -> " +
                                 std::to_string(b2.seq) + ", copies " + std::to_string(b1.cps) +
                                 " -> " + std::to_string(b2.cps) + " in " +
                                 std::to_string(o.beat_ms) + " ms",
                             RC_ACTIVATE_FAILED);
    log_note("ring: seq %u -> %u, copies %u -> %u, blocks %u -> %u (%d ms)", b1.seq, b2.seq,
             b1.cps, b2.cps, b1.blks, b2.blks, o.beat_ms);

    PllInfo pll2 = read_pll(w);
    if (!pll_matches(pll2, map, &err))
        return activate_fail(w, map, state_path, &st, "the PLL does not keep the values: " + err,
                             RC_ACTIVATE_FAILED);
    RailInfo rails2 = read_rails(w);
    for (int i = 0; i < 3; ++i) {
        if (!rails2.valid || (rails2.con0[i] & map.rail_bit) == 0)
            return activate_fail(w, map, state_path, &st,
                                 std::string("rail ") + rail_specs[i].name +
                                     " is not powered at the end (CON0 " + hex32(rails2.con0[i]) + ")",
                                 RC_ACTIVATE_FAILED);
    }
    log_note("rails: VRF18=%s VFE28=%s VRFCK=%s", hex32(rails2.con0[0]).c_str(),
             hex32(rails2.con0[1]).c_str(), hex32(rails2.con0[2]).c_str());
    log_note("PLL: channel %u, band %u, %u kHz (preserved)", pll2.channel, pll2.band, pll2.khz);

    // 9. Summary.
    log_step(++step, steps, "summary");
    st.state = "sdr-active";
    st.loop_armed = true;
    st.rails_on = true;
    st.last_error.clear();
    st.mode_profile = map.name;
    if (persist_or_fail(state_path, st, "closing the activation")) return RC_USAGE;
    log_note("state: %s", state_brief(st).c_str());
    log_note("stop the loops     : sdr-load --deactivate");
    log_note("go all the way back: sdr-load --restore");
    std::printf("result: SDR mode activated (PLL, rails and loops verified)\n");
    return RC_OK;
}

// --resume: pick up where the last operation stopped. It never writes the partition: it
// checks the image that is already there and finishes the activation (or the revert) that
// a reboot interrupted.
int action_resume(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    std::printf("%s --resume\n", kToolVersion);
    State st;
    std::string err;
    const StateRead sr = state_read(state_path, &st, &err);
    if (sr == StateRead::Missing) {
        log_error("there is no state file at %s: nothing to resume", state_path.c_str());
        log_note("to see where the phone is: sdr-load --status");
        return RC_PRECONDITION;
    }
    if (sr == StateRead::Unreadable) {
        log_error("the state file %s cannot be read: %s", state_path.c_str(), err.c_str());
        if (running_as_root())
            log_note("if it is corrupt, move it away and start from scratch;"
                     " nothing is touched here");
        else
            log_note("run it with privileges: sudo sdr-load --resume");
        return RC_PRECONDITION;
    }
    log_note("previous state: %s", state_brief(st).c_str());

    // A revert that stopped halfway is closed by --verify-boot: it is a diagnosis, not an
    // activation.
    if (st.pending == "reboot-to-recover" || st.state == "recovering" ||
        st.state == "restoring" || st.state == "flashing") {
        log_note("the last operation was a revert: closing it with --verify-boot");
        return action_verify_boot(o);
    }

    // Before deciding what is missing: which image is in the partition? The state can
    // say "sdr-active" (or "activating") about an image that was written back, and then
    // "the reception is running" or "finishing the activation" are both wrong. The two
    // causes are named: the image was reverted, or the image is right and only the
    // activation is missing.
    const PartitionFacts pf = partition_facts(o, st);
    const ImageTruth truth = image_truth(st, pf);
    if (truth.verdict == ImageVerdict::Incoherent) {
        log_error("%s", truth.text.c_str());
        log_note("%s", truth.hint.c_str());
        log_note("nothing is resumed on an image that is not the one of the mode");
        return RC_SHA_MISMATCH;
    }
    const bool image_ok = truth.verdict == ImageVerdict::Coherent && !pf.profile.empty();
    if (image_ok) {
        log_note("image: %s (%s): the image was not reverted", short_sha(pf.sha256).c_str(),
                 image_kind_text(pf).c_str());
    } else if (!pf.readable) {
        log_warn("the image in the partition cannot be checked here (%s)", pf.err.c_str());
    } else {
        log_warn("the image in the partition does not match a mode profile (%s)",
                 image_kind_text(pf).c_str());
    }

    const bool pending_enter = st.pending == "reboot-to-enter";
    const bool interrupted = st.state == "activating" || st.state == "activate-failed" ||
                             st.state == "entering" ||
                             (st.state == "sdr-active" && !st.loop_armed);
    if (!pending_enter && !interrupted) {
        if (st.state == "sdr-active" && st.loop_armed) {
            log_note("the reception is already running: nothing to resume");
            log_note("stop it with: sdr-load --deactivate");
        } else {
            log_note("nothing to resume: the state is %s", st.state.c_str());
            log_note("to start the reception: sdr-load --activate");
        }
        std::printf("result: nothing to resume\n");
        return RC_OK;
    }
    if (pending_enter) {
        if (!reboot_happened(st)) {
            log_error("the flash is still pending reboot (state %s)", st.state.c_str());
            log_note("reboot with: sudo systemctl reboot");
            log_note("afterwards: sudo sdr-load --resume (it continues without writing the image)");
            return RC_REBOOT_NEEDED;
        }
        log_note("reboot detected: the activation continues without writing the image again");
    } else if (image_ok) {
        log_note("the image is in place: what is missing is the activation (loops/sweep): resuming it");
    } else {
        log_note("activation interrupted at '%s': finishing it", st.state.c_str());
    }

    Options ao = o;
    ao.via_resume = true;
    return action_activate(ao);
}

int action_deactivate(const Options& o) {
    const std::string state_path = o.state_path.empty() ? default_state_path() : o.state_path;
    std::printf("%s --deactivate\n", kToolVersion);
    int step = 0;
    const int steps = 5;

    State st;
    std::string err;
    st.loaded = state_load(state_path, &st, &err);
    if (!st.loaded) st = State();

    log_step(++step, steps, "image and profile: stop mode");
    ModeMap map;
    bool identified = false;
    if (!resolve_mode_map(o, st, &map, &err, &identified)) {
        if (o.dry_run) {
            log_warn("cannot identify the image here (%s)", err.c_str());
            log_note("the plan is shown with the first known profile");
            const ImageProfile* ex = find_profile_by_name("standard-a");
            if (!ex) return RC_NO_PROFILE;
            map = map_from_profile(*ex);
        } else {
            log_error("%s", err.c_str());
            log_note("known profiles: %s", known_profile_names().c_str());
            return RC_NO_PROFILE;
        }
    }
    log_note("profile: %s (%s)", map.name.c_str(), map.source.c_str());
    g_profile_used = map.name;

    if (map.scan) {
        if (o.dry_run) {
            log_step(++step, steps, "stop plan (dry-run)");
            log_note("(dry-run) cut the scan with AT+CFUN=0 / AT+CFUN=1");
            log_note("(dry-run) this image arms no loops: there is nothing else to stop");
            std::printf("result: dry-run: nothing has been touched\n");
            return RC_OK;
        }
        if (!running_as_root()) {
            log_error("root is required to talk to the modem");
            return RC_PRECONDITION;
        }
        log_step(++step, steps, "scan: cut it with AT+CFUN=0 / AT+CFUN=1");
        std::string out;
        const bool a = at_send("AT+CFUN=0", &out, 20);
        ::usleep(1000 * 1000);
        const bool b = at_send("AT+CFUN=1", &out, 20);
        if (!a && !b)
            log_warn("could not cut it over AT (%s); the image has no loops to stop",
                     out.c_str());
        else
            log_note("scan cut (CFUN=0 %s, CFUN=1 %s)", a ? "OK" : "no OK",
                     b ? "OK" : "no OK");
        st.state = "sdr-active";
        st.loop_armed = false;
        st.rails_on = false;
        st.last_error.clear();
        st.mode_profile = map.name;
        if (persist_or_fail(state_path, st, "closing the stop")) return RC_USAGE;
        log_note("state: %s", state_brief(st).c_str());
        log_note("to go all the way back (radios and firmware): sdr-load --restore");
        std::printf("result: scan cut; this image has no loops\n");
        return RC_OK;
    }

    if (map.passive) {
        log_error("image %s has no loops to stop: it dispatches no modes", map.name.c_str());
        log_note("to leave SDR mode and give the phone back: sdr-load --restore");
        return RC_NO_PROFILE;
    }

    if (o.dry_run) {
        log_step(++step, steps, "stop plan (dry-run)");
        for (const auto& s : plan_deactivate(map)) log_note("(dry-run) %s", s.c_str());
        std::printf("result: dry-run: nothing has been touched\n");
        return RC_OK;
    }

    if (!running_as_root()) {
        log_error("root is required (modem command block at %s)", kWindowNode);
        return RC_PRECONDITION;
    }

    log_step(++step, steps, "window: open and read the loop state");
    Window w;
    if (!w.open_rw(&err)) {
        log_error("cannot open %s: %s", kWindowNode, err.c_str());
        return RC_PRECONDITION;
    }
    RailInfo rails0 = read_rails(w);
    TimingLoopInfo tl0 = read_timing_loop(w);
    log_note("before: X3_ARMED=%s PASS=%u | X4_ARMED=%s PASS=%u",
             rails0.valid ? hex32(rails0.armed).c_str() : "no markers", rails0.pass,
             tl0.valid ? hex32(tl0.armed).c_str() : "no markers", tl0.pass);

    log_step(++step, steps, "stop the loops (mode %d: does not touch the PMIC)", map.loop_stop);
    ModeResult r = request_mode(w, map, map.loop_stop, o.mode_timeout_ms);
    if (!r.ok) {
        log_error("could not request the stop: %s", r.err.c_str());
        st.last_error = r.err;
        persist_or_fail(state_path, st, "recording the failure");
        return RC_ACTIVATE_FAILED;
    }
    uint32_t a3 = 1, a4 = 1, s3 = 0, s4 = 0;
    wit_read(w, kMarkX3Armed, &a3);
    wit_read(w, kMarkX4Armed, &a4);
    wit_read(w, kMarkX3Stop, &s3);
    wit_read(w, kMarkX4Stop, &s4);
    // Only the loops this image really has are required to disarm.
    const bool still3 = rails0.valid && a3 != 0;
    const bool still4 = tl0.valid && a4 != 0;
    if (still3 || still4) {
        log_error("the loop is still armed: X3_ARMED=%s X4_ARMED=%s", hex32(a3).c_str(),
                  hex32(a4).c_str());
        st.last_error = "the stop mode did not disarm the loops";
        persist_or_fail(state_path, st, "recording the failure");
        return RC_ACTIVATE_FAILED;
    }
    log_note("loops stopped: X3_ARMED=%s X4_ARMED=%s (stops %u/%u)", hex32(a3).c_str(),
             hex32(a4).c_str(), s3, s4);

    log_step(++step, steps, "rails: record (they are not powered down in software)");
    RailInfo rails1 = read_rails(w);
    if (rails1.valid) {
        log_note("CON0: VRF18=%s VFE28=%s VRFCK=%s", hex32(rails1.con0[0]).c_str(),
                 hex32(rails1.con0[1]).c_str(), hex32(rails1.con0[2]).c_str());
    } else {
        log_note("CON0: no loop markers (the rail state cannot be read)");
    }
    log_warn("the rails are NOT powered down in software: CON0 is only ever written with |= 1, never 0");
    log_note("they stay powered until the modem is restarted or power is cut;");
    log_note("to leave SDR mode and give the phone back: sdr-load --restore");

    log_step(++step, steps, "summary");
    st.loop_armed = false;
    st.rails_on = rails1.valid && (rails1.con0[0] & map.rail_bit) && (rails1.con0[1] & map.rail_bit) &&
                  (rails1.con0[2] & map.rail_bit);
    if (st.state == "activating" || st.state == "activate-failed") st.state = "sdr-active";
    st.last_error.clear();
    st.mode_profile = map.name;
    if (persist_or_fail(state_path, st, "closing the stop")) return RC_USAGE;
    log_note("state: %s", state_brief(st).c_str());
    std::printf("result: loops stopped; rails untouched (they are not powered down in software)\n");
    return RC_OK;
}

int action_modes(const Options& o) {
    std::printf("%s --modes\n", kToolVersion);
    if (!o.mode_profile.empty()) {
        const bool looks_path = o.mode_profile.find('/') != std::string::npos ||
                                o.mode_profile.find(".conf") != std::string::npos ||
                                o.mode_profile.find(".txt") != std::string::npos;
        if (looks_path) {
            ModeMap map;
            std::string err;
            if (!load_profile_file(o.mode_profile, &map, &err)) {
                log_error("%s", err.c_str());
                return RC_NO_PROFILE;
            }
            std::string why;
            if (!map.complete(&why)) log_warn("%s", why.c_str());
            print_mode_table(map);
            return RC_OK;
        }
        const ImageProfile* p = find_profile_by_name(o.mode_profile);
        if (!p) {
            log_error("unknown profile: '%s' (known: %s)", o.mode_profile.c_str(),
                      known_profile_names().c_str());
            return RC_NO_PROFILE;
        }
        print_mode_table(map_from_profile(*p));
        return RC_OK;
    }
    for (const auto& p : known_profiles()) print_mode_table(map_from_profile(p));
    log_note("for a profile file: sdr-load --modes --profile <path>");
    return RC_OK;
}

// ---------------------------------------------------------------------------
// Self-test (no device, every path that does not mutate anything)
// ---------------------------------------------------------------------------

// Defined after this function, next to main(): declared here because the self-test
// exercises the option parsing too.
bool parse_args(int argc, char** argv, Options* o, std::string* err);

int action_selftest(const Options& o) {
    int checks = 0, ok = 0;
    auto expect = [&](bool cond, const char* what) {
        ++checks;
        if (cond) {
            ++ok;
            std::printf("  ok   %s\n", what);
        } else {
            std::printf("  FAIL %s\n", what);
        }
    };

    std::printf("%s --selftest\n", kToolVersion);

    expect(sha256_of_string("") ==
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "sha256 of the empty string");
    expect(sha256_of_string("abc") ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "sha256 de \"abc\"");
    expect(sha256_of_string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "sha256 of the 56-byte vector");

    std::vector<uint32_t> a{1, 2, 3, 4, 5}, b{1, 2, 3, 4, 5}, c{1, 0, 3, 4, 5};
    RingDiff d0 = diff_snapshots({a}, {b});
    RingDiff d1 = diff_snapshots({a}, {c});
    expect(d0.changed_words == 0, "identical rings -> 0 changes");
    expect(d1.changed_words == 1, "one differing word -> 1 change");
    expect(d1.bytes_sampled == 20, "sampled bytes counted");

    // State round-trip in a temporary directory.
    const std::string tmpdir = o.state_path.empty() ? "/tmp/sdr-load-selftest" : o.state_path;
    const std::string tmpstate = tmpdir + "/state.json";
    State s;
    s.state = "sdr-active";
    s.pending = "reboot-to-recover";
    s.image = "/path/with \"quotes\" and \\ slash";
    s.image_sha256 = std::string(64, 'a');
    s.image_size = 63018064;
    s.partition = "/dev/disk/by-partlabel/x";
    s.wifi_blocked_by_tool = true;
    s.bt_blocked_by_tool = false;
    s.last_error = "line1\nline2";
    std::string err;
    expect(state_save(tmpstate, s, &err), "save state (json)");
    State t;
    expect(state_load(tmpstate, &t, &err), "load state (json)");
    expect(t.state == s.state && t.pending == s.pending, "state/pending preserved");
    expect(t.image == s.image, "paths with quotes and slashes preserved");
    expect(t.image_sha256 == s.image_sha256, "sha256 preserved");
    expect(t.image_size == s.image_size, "image size preserved");
    expect(t.wifi_blocked_by_tool && !t.bt_blocked_by_tool, "radio flags preserved");
    expect(t.last_error == s.last_error, "error message with a line break");
    ::unlink(tmpstate.c_str());

    State none;
    expect(!state_load(tmpstate, &none, &err), "missing state -> not loaded");
    expect(!none.loaded, "missing state -> loaded=false");
    Options so;
    State sst;
    sst.loaded = true;
    sst.image_size = 4096;
    expect(expected_image_size(so, sst) == 4096, "expected size from the state");
    so.image_size = 8192;
    expect(expected_image_size(so, sst) == 8192, "--size wins over the state");
    State bs;
    bs.boot_id = "previous-boot";
    expect(reboot_happened(bs), "different boot_id -> reboot detected");
    bs.boot_id = current_boot_id();
    expect(!reboot_happened(bs) || current_boot_id().empty(), "current boot_id -> no reboot");

    // One canonical state path, and the old per-user copy only as a read fallback.
    ::unsetenv("SDR_LOAD_STATE");
    expect(default_state_path() == kDefaultStatePath, "one canonical state path for every caller");
    ::setenv("SDR_LOAD_STATE", "/tmp/sdr-load-selftest/other.json", 1);
    expect(default_state_path() == "/tmp/sdr-load-selftest/other.json",
           "SDR_LOAD_STATE still overrides the canonical path");
    ::unsetenv("SDR_LOAD_STATE");
    const char* home_env = std::getenv("HOME");
    const std::string home_saved = home_env ? home_env : "";
    ::setenv("HOME", "/tmp/sdr-load-selftest-home", 1);
    expect(legacy_state_path() == "/tmp/sdr-load-selftest-home/.local/state/sdr-load/state.json",
           "the legacy per-user path comes from HOME and is only read");

    auto write_text = [](const std::string& path, const std::string& text) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        return ok;
    };

    State s0;
    std::string e0;
    mkdir_p(tmpdir);
    expect(state_read(tmpdir + "/nope.json", &s0, &e0) == StateRead::Missing,
           "a missing state file is Missing, never a default state");
    const std::string adir = tmpdir + "/a-directory";
    ::mkdir(adir.c_str(), 0755);
    expect(state_read(adir, &s0, &e0) == StateRead::Unreadable,
           "a state path that cannot be read is Unreadable");
    const std::string junk = tmpdir + "/junk.json";
    expect(write_text(junk, "{\"hello\": 1}\n"), "write a file that is not a state");
    expect(state_read(junk, &s0, &e0) == StateRead::Unreadable && !s0.loaded,
           "a file without a state key is not taken as a state");
    mkdir_p("/tmp/sdr-load-selftest-home/.local/state/sdr-load");
    expect(write_text(legacy_state_path(), "{\"state\": \"sdr-active\", \"pending\": \"\"}\n"),
           "write a legacy per-user state");
    expect(state_read(tmpdir + "/nope.json", &s0, &e0) == StateRead::Missing,
           "an explicit --state path is never replaced by the legacy file");
    if (!home_saved.empty()) ::setenv("HOME", home_saved.c_str(), 1);

    // The image in the partition has to match what the state recorded: that is what lets
    // the activation go on after a reboot without writing anything.
    const std::string img = tmpdir + "/image.bin";
    expect(write_text(img, "abc"), "write a stand-in image");
    Options io;
    io.partition = img;
    State is;
    is.image_size = 3;
    is.image_sha256 = sha256_of_string("abc");
    ImageCheck ic = check_partition_image(io, is);
    expect(ic.ok && ic.readable && ic.sha == is.image_sha256,
           "the partition holds the recorded image");
    is.image_sha256 = sha256_of_string("abd");
    ic = check_partition_image(io, is);
    expect(!ic.ok && ic.readable, "another sha256 -> the partition does not match");
    is.image_sha256.clear();
    expect(!check_partition_image(io, is).ok,
           "no sha256 in the state -> the image cannot be verified");
    is.image_sha256 = sha256_of_string("abc");
    is.image_size = 4;
    ic = check_partition_image(io, is);
    expect(!ic.ok && ic.readable, "another size -> the partition does not match");
    is.image_size = 3;
    expect(!check_partition_image(Options{}, is).ok,
           "no partition to read -> it cannot be verified");

    // The state never wins over the partition: a state that says sdr-active for a
    // partition that holds the stock image is incoherent, and the message says exactly
    // that instead of reporting the state as good.
    const std::string stock_sha =
        "22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896";
    const std::string sweep_sha =
        "5a625327a06fcacf12f611419437bc85aa54c339911a8d3427262fea0643b4d3";
    PartitionFacts f_stock;
    f_stock.resolved = f_stock.readable = true;
    f_stock.partition = "/dev/disk/by-partlabel/md1img_a";
    f_stock.limit = f_stock.size = 63018064;
    f_stock.sha256 = stock_sha;
    f_stock.stock = true;
    expect(image_kind_text(f_stock) == "stock image", "a stock partition is named as such");
    State st_sdr;
    st_sdr.loaded = true;
    st_sdr.state = "sdr-active";
    st_sdr.image_sha256 = stock_sha;
    st_sdr.image_size = 63018064;
    ImageTruth it = image_truth(st_sdr, f_stock);
    expect(it.verdict == ImageVerdict::Incoherent,
           "state sdr-active over the stock image -> incoherent");
    expect(it.text.find("state says sdr-active") != std::string::npos &&
               it.text.find("stock image") != std::string::npos &&
               it.text.find("22c1909d") != std::string::npos,
           "the verdict names the state, the image kind and the sha256");
    expect(it.hint.find("--image") != std::string::npos,
           "the way out of the incoherence is to write the image again");

    PartitionFacts f_sweep = f_stock;
    f_sweep.sha256 = sweep_sha;
    f_sweep.stock = false;
    f_sweep.profile = "sdr-8bit";
    State st_ok = st_sdr;
    st_ok.image_sha256 = sweep_sha;
    it = image_truth(st_ok, f_sweep);
    expect(it.verdict == ImageVerdict::Coherent && it.matches_state,
           "state sdr-active over the image it recorded -> coherent");
    expect(it.text.find("mode image sdr-8bit") != std::string::npos,
           "the coherent verdict names the mode image");
    it = image_truth(st_sdr, f_sweep);
    expect(it.verdict == ImageVerdict::Incoherent &&
               it.text.find("the state recorded 22c1909d") != std::string::npos,
           "another mode image than the recorded one -> incoherent, both named");
    State st_ok2;
    st_ok2.loaded = true;
    st_ok2.state = "ok";
    st_ok2.image_sha256 = stock_sha;
    st_ok2.image_size = 63018064;
    it = image_truth(st_ok2, f_stock);
    expect(it.verdict == ImageVerdict::Coherent && it.text.find("stock image") != std::string::npos,
           "the stock image with a state that says ok is coherent");
    PartitionFacts f_unreadable;
    f_unreadable.partition = "/dev/disk/by-partlabel/md1img_a";
    f_unreadable.err = "cannot read /dev/disk/by-partlabel/md1img_a: Permission denied";
    it = image_truth(st_sdr, f_unreadable);
    expect(it.verdict == ImageVerdict::Unverified &&
               it.text.find("Permission denied") != std::string::npos,
           "without privileges the image is reported as unverified, never as good");
    expect(it.hint.find("privileges") != std::string::npos,
           "the unverified image says how to read it");
    PartitionFacts f_whole = f_stock;
    f_whole.whole = true;
    f_whole.limit = 0;
    f_whole.size = 209715200;
    f_whole.stock = false;
    it = image_truth(st_sdr, f_whole);
    expect(it.verdict == ImageVerdict::Unverified,
           "an unknown image size leaves the whole partition hashed -> unverified");

    // Code 12 has two different causes and they do not share a message.
    const std::string sweep_profile = "sdr-8bit";
    MarkDiagnosis md = diagnose_missing_mark(f_sweep, sweep_sha, sweep_profile, 45000,
                                             "is the sweep running?");
    expect(md.rc == RC_NO_DUMP, "right image and no mark -> 12, not a mismatch");
    expect(md.text.find("the image is correct") != std::string::npos &&
               md.text.find("45 s") != std::string::npos &&
               md.text.find("is the sweep running?") != std::string::npos,
           "12 says the image is right and asks whether the sweep is running");
    md = diagnose_missing_mark(f_stock, sweep_sha, sweep_profile, 45000, "");
    expect(md.rc == RC_SHA_MISMATCH, "stock image and no mark -> 5, the image is not in place");
    expect(md.text.find("not in place") != std::string::npos &&
               md.text.find("stock image") != std::string::npos &&
               md.text.find("--image") != std::string::npos,
           "the mismatch names the stock image and says to write the mode image");
    md = diagnose_missing_mark(f_unreadable, sweep_sha, sweep_profile, 45000, "");
    expect(md.rc == RC_NO_DUMP && md.text.find("could be checked") != std::string::npos,
           "an unreadable partition does not turn 12 into a sha mismatch");

    // --mark-timeout-ms is what keeps a silence from being read as a missing mark.
    {
        Options mo;
        std::string perr2;
        const char* argv[] = {"sdr-load", "--activate", "--mark-timeout-ms", "7000"};
        expect(parse_args(4, const_cast<char**>(argv), &mo, &perr2) && mo.mark_timeout_ms == 7000,
               "--mark-timeout-ms sets the window used before code 12");
        const char* argv2[] = {"sdr-load", "--activate", "--mark-timeout-ms", "abc"};
        Options mo2;
        expect(parse_args(4, const_cast<char**>(argv2), &mo2, &perr2) && mo2.mark_timeout_ms == 0,
               "a non-numeric mark timeout is clamped to 0, not left negative");
    }

    // pending=reboot-to-enter is closed after the reboot, and only on the recorded image.
    const std::string rstate = tmpdir + "/resume.json";
    State rs;
    rs.state = "ok";
    rs.pending = "reboot-to-enter";
    rs.boot_id = "previous-boot";
    rs.image = img;
    rs.image_size = 3;
    rs.image_sha256 = sha256_of_string("abc");
    expect(state_save(rstate, rs, &err), "save a state with a pending reboot");
    State resumed = rs;
    const bool q_was = g_quiet;
    g_quiet = true;  // the resume logs for a human, not for the test output
    const int rc_resumed = resume_pending_flash(io, rstate, &resumed);
    g_quiet = q_was;
    expect(rc_resumed == RC_OK, "a verified image resumes the activation after the reboot");
    expect(resumed.pending.empty() && resumed.state == "ok", "the pending flag is closed");
    State back;
    expect(state_load(rstate, &back, &err) && back.pending.empty(),
           "the closed flash is persisted");
    std::string img_now;
    uint64_t img_size = 0;
    expect(sha256_of_file(img, 0, &img_now, &img_size, &err) && img_now == sha256_of_string("abc"),
           "the image in the partition is not written again");

    // Plans: the three destructive paths must name their key steps.
    Options po = o;
    po.image = "/tmp/x.img";
    auto has = [](const std::vector<std::string>& v, const char* needle) {
        for (const auto& s2 : v)
            if (s2.find(needle) != std::string::npos) return true;
        return false;
    };
    auto pl = plan_load(o, State{});
    expect(has(pl, "0x430E") && has(pl, "rings"), "load plan: ioctl and rings");
    auto pf = plan_flash(po);
    expect(has(pf, "sha256") && has(pf, "flashing"), "flash plan: hash and previous state");
    State sr;
    sr.loaded = true;
    sr.wifi_blocked_by_tool = true;
    auto pr = plan_restore(o, sr);
    expect(has(pr, "0x430F") && has(pr, "recovering") && has(pr, "recovery"),
           "revert plan: exit, recovery and image");
    Options prb = o;
    prb.no_reboot = true;
    expect(has(plan_restore(prb, sr), "code 10"), "revert plan: no reboot -> code 10");

    // Per-image profiles: table, file and refusal when the image is not there.
    expect(known_profiles().size() >= 2, "there are at least two image profiles");
    bool profiles_ok = true;
    for (const auto& p : known_profiles()) {
        std::string why;
        ModeMap m = map_from_profile(p);
        if (p.sha256.size() != 64 || !m.complete(&why)) profiles_ok = false;
    }
    expect(profiles_ok, "the known profiles declare sha256 and all their modes");
    const ImageProfile* std_a = find_profile_by_name("standard-a");
    expect(std_a != nullptr && find_profile_by_sha256(std_a->sha256) == std_a,
           "the reference profile is found by name and by sha256");
    expect(find_profile_by_sha256(std::string(64, '0')) == nullptr,
           "an unknown sha256 has no profile");

    // sdr-8bit: 8 bits per component, origin 3, tag and fixed tuning at 2,115.0 MHz.
    // It dispatches no modes, but it is activated by starting the firmware scan.
    const ImageProfile* sdr8 = find_profile_by_name("sdr-8bit");
    expect(sdr8 != nullptr && sdr8->sha256.size() == 64 &&
               sdr8->sha256 == "5a625327a06fcacf12f611419437bc85aa54c339911a8d3427262fea0643b4d3",
           "8-bit image: profile with the measured image sha256");
    expect(sdr8 != nullptr && find_profile_by_sha256(sdr8->sha256) == sdr8,
           "8-bit image found by name and by sha256");
    std::string why8;
    expect(sdr8 != nullptr && sdr8->scan && map_from_profile(*sdr8).complete(&why8),
           "8-bit image is a sweep image and complete() requires no modes from it");
    expect(sdr8 != nullptr && sdr8->fixed_khz == 21150 && sdr8->fixed_band == 1,
           "8-bit image declares fixed tuning 2,115.0 MHz band 1");
    expect(sdr8 != nullptr && sdr8->cave_max == 0 && sdr8->pll_seed < 0 && sdr8->arm_addr == 0,
           "8-bit image declares no mode sequence");
    expect(std_a == nullptr || (!std_a->passive && !std_a->scan),
           "the reference profile is still a mode one");
    ModeMap pf_passive;
    expect(parse_profile_text("name = passive\npassive = 1\n", "passive.conf", &pf_passive, &err) &&
               pf_passive.passive && pf_passive.complete(&why8),
           "passive profile with no modes: it loads and requires no sequence");
    ModeMap pf_scan;
    expect(parse_profile_text("name = sweep\nscan = 1\nfixed_khz = 21150\nfixed_band = 1\n",
                              "sweep.conf", &pf_scan, &err) &&
               pf_scan.scan && pf_scan.fixed_khz == 21150 && pf_scan.complete(&why8),
           "sweep profile with no modes: it loads, keeps the frequency and requires no sequence");

    const std::string prof_text =
        "# test profile\n"
        "name = test\n"
        "sha256 = " +
        std::string(64, 'b') +
        "\n"
        "pll_seed = 3\n"
        "rail_v18 = 27\nrail_v28 = 28\nrail_vck = 29\n"
        "keepalive = 30\nloop_stop = 31\ntiming = 32\n"
        "peek_mode = 5\npoke_mode = 6\n"
        "arm_addr = 0x255C6B58\narm_value = 0x100\n"
        "pll_channel = 4\npll_band = 77\npll_khz = 4070010\n"
        "loop_period = 100\ncdf0 = 256\ncdf1 = 656\nbsi = 256\n"
        "mode = 3: seeds the PLL and queues RX ON\n"
        "mode = 31: stops both loops\n";
    ModeMap pm;
    expect(parse_profile_text(prof_text, "test.conf", &pm, &err), "profile file: it loads");
    expect(pm.arm_addr == 0x255C6B58 && pm.pll_khz == 4070010,
           "profile file: hex and decimal numbers");
    expect(pm.modes.size() == 2 && pm.meaning(31).find("loops") != std::string::npos,
           "profile file: mode table");
    std::string pwhy;
    expect(!pm.complete(&pwhy), "incomplete profile file -> complete() says so");
    expect(pm.cave_max == 31, "profile file: mode ceiling inferred");
    ModeMap bad;
    expect(!parse_profile_text("pll_seed = 3\ninventada = 1\n", "bad.conf", &bad, &err),
           "profile with an unknown key -> error");
    expect(parse_profile_text("mode = 3: x\n", "bad2.conf", &bad, &err) && !bad.complete(&pwhy),
           "profile without the sequence -> not usable for activation");
    const ImageProfile empty_profile;
    const ImageProfile& prof_std_a = std_a ? *std_a : empty_profile;
    auto pa = plan_activate(o, map_from_profile(prof_std_a));
    expect(has(pa, "CON0 |= 1") && has(pa, "KPL1") && has(pa, "KPL2"),
           "activation plan: CON0 rule and loops");
    expect(has(pa, "WITHOUT writing"),
           "activation plan: a pending flash is closed without writing");
    auto pd = plan_deactivate(map_from_profile(prof_std_a));
    expect(has(pd, "does not touch the PMIC") && has(pd, "NOT powered down"),
           "stop plan: PMIC and rails");

    // Real probes, read-only: they must not crash or mutate anything.
    RadiosReport rr = radios_report();
    expect(rr.entries.size() < 64, "rfkill scan stable");
    expect(up_interfaces_with_prefix("ccmni").size() < 1024, "interface scan stable");
    Window w;
    std::string werr;
    bool opened = w.open(&werr);
    std::printf("  window info: %s%s%s\n", opened ? "available" : "not available (",
                opened ? "" : werr.c_str(), opened ? "" : ")");

    std::printf("%d/%d checks OK\n", ok, checks);
    return ok == checks ? RC_OK : RC_USAGE;
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

void usage() {
    std::printf(
        "%s -- load, activation and revert of SDR mode\n"
        "\n"
        "usage:\n"
        "  sdr-load [--check]                 report; touches nothing (default)\n"
        "  sdr-load --image <path> [--reboot] flash the image, verify sha256 and ask for a reboot\n"
        "  sdr-load --no-flash [--sha256 H]   enter SDR mode with the image already loaded\n"
        "  sdr-load --activate                activate the mode: PLL, rails and loops, step by step\n"
        "  sdr-load --resume                  finish what a reboot interrupted (no new write)\n"
        "  sdr-load --deactivate              stop the loops (the rails are NOT powered down in software)\n"
        "  sdr-load --restore                 leave the mode, restore radios and modem\n"
        "  sdr-load --status                  persisted state + what is observed now\n"
        "  sdr-load --modes [--profile P]     mode table -> meaning for one image\n"
        "  sdr-load --verify-boot             close or diagnose after a reboot\n"
        "  sdr-load --selftest                tests without a device\n"
        "\n"
        "options:\n"
        "  --check                 read-only report (default action)\n"
        "  --image <path>          firmware image to flash (through the flash script)\n"
        "  --no-flash              do not flash: the image is already loaded\n"
        "  --sha256 <hex>          expected sha256 (of the image or of the partition)\n"
        "  --size <bytes>          usable bytes of the partition (def. the state ones)\n"
        "  --profile <name|path>   mode profile: %s, or a file (key = value)\n"
        "  --restore               revert (clean and, if needed, with recovery)\n"
        "  --recover-image <path>  good image for the revert (def. %s)\n"
        "  --flash-script <path>   flash script (def. %s)\n"
        "  --partition <path>      firmware partition (def. the one in the script)\n"
        "  --state <path>          state file (def. %s)\n"
        "  --reboot                reboot when the plan asks for it\n"
        "  --no-reboot             never reboot; exit with code 10\n"
        "  --yes                   do not ask (for non-interactive use)\n"
        "  --dry-run               print the plan without running it\n"
        "  --json                  final summary in JSON\n"
        "  --quiet                 errors only\n"
        "  --settle-ms <n>         grace before the first ring sample (def. 1500)\n"
        "  --quiet-timeout-ms <n>  wait for the rings to go quiet after the ioctl (def. 20000)\n"
        "  --gap-ms <n>            separation between ring samples (def. 1000)\n"
        "  --samples <n>           ring samples (def. 2)\n"
        "  --wait-ms <n>           maximum wait for the modem when reverting (def. 20000)\n"
        "  --md1-timeout-ms <n>    wait for md1.status=ready on activation (def. 60000)\n"
        "  --mode-timeout-ms <n>   wait per mode requested from the modem (def. 90000)\n"
        "  --beat-ms <n>           separation between the two ring samples (def. 2000)\n"
        "  --mark-timeout-ms <n>   window to look for the dump-engine mark before code 12\n"
        "                          (def. 45000: the modem publishes in bursts)\n"
        "  -h, --help              this help\n"
        "\n"
        "exit codes: 0 ok, 1 usage, 2 precondition, 3 radios, 4 flash, 5 sha256,\n"
        "6 reboot needed, 7 ioctl, 8 mode not verified, 9 incomplete revert,\n"
        "10 recovery flashed (reboot needed), 11 rebooting, 12 no dump,\n"
        "13 image without a mode profile, 14 activation failed (loop disarmed).\n",
        kToolVersion, known_profile_names().c_str(), kDefaultRecoverImage, kDefaultFlashScript,
        kDefaultStatePath);
}

bool parse_args(int argc, char** argv, Options* o, std::string* err) {
    auto need = [&](int i, const std::string& what) -> bool {
        if (i + 1 >= argc) {
            *err = std::string("missing the value of ") + what;
            return false;
        }
        return true;
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--check") o->action = Action::Check;
        else if (a == "--no-flash") o->action = Action::Load;
        else if (a == "--activate") o->action = Action::Activate;
        else if (a == "--resume") o->action = Action::Resume;
        else if (a == "--deactivate") o->action = Action::Deactivate;
        else if (a == "--modes") o->action = Action::Modes;
        else if (a == "--restore") o->action = Action::Restore;
        else if (a == "--status") o->action = Action::Status;
        else if (a == "--verify-boot") o->action = Action::VerifyBoot;
        else if (a == "--selftest") o->action = Action::SelfTest;
        else if (a == "-h" || a == "--help") o->action = Action::Help;
        else if (a == "--image") {
            if (!need(i, a)) return false;
            o->image = argv[++i];
            o->action = Action::Flash;
        } else if (a == "--size") {
            if (!need(i, a)) return false;
            o->image_size = std::strtoull(argv[++i], nullptr, 0);
        } else if (a == "--sha256") {
            if (!need(i, a)) return false;
            o->image_sha256_expected = trim(argv[++i]);
            o->no_flash_sha256 = o->image_sha256_expected;
        } else if (a == "--recover-image") {
            if (!need(i, a)) return false;
            o->recover_image = argv[++i];
        } else if (a == "--flash-script") {
            if (!need(i, a)) return false;
            o->flash_script = argv[++i];
        } else if (a == "--partition") {
            if (!need(i, a)) return false;
            o->partition = argv[++i];
        } else if (a == "--state") {
            if (!need(i, a)) return false;
            o->state_path = argv[++i];
        } else if (a == "--profile") {
            if (!need(i, a)) return false;
            o->mode_profile = argv[++i];
        } else if (a == "--md1-timeout-ms") {
            if (!need(i, a)) return false;
            o->md1_timeout_ms = std::atoi(argv[++i]);
        } else if (a == "--mode-timeout-ms") {
            if (!need(i, a)) return false;
            o->mode_timeout_ms = std::atoi(argv[++i]);
        } else if (a == "--beat-ms") {
            if (!need(i, a)) return false;
            o->beat_ms = std::atoi(argv[++i]);
        } else if (a == "--mark-timeout-ms") {
            if (!need(i, a)) return false;
            o->mark_timeout_ms = std::atoi(argv[++i]);
        } else if (a == "--reboot") o->reboot = true;
        else if (a == "--no-reboot") o->no_reboot = true;
        else if (a == "--yes" || a == "-y") o->assume_yes = true;
        else if (a == "--dry-run") o->dry_run = true;
        else if (a == "--json") o->json = true;
        else if (a == "--quiet") o->quiet = true;
        else if (a == "--settle-ms") {
            if (!need(i, a)) return false;
            o->settle_ms = std::atoi(argv[++i]);
        } else if (a == "--quiet-timeout-ms") {
            if (!need(i, a)) return false;
            o->quiet_timeout_ms = std::atoi(argv[++i]);
        } else if (a == "--gap-ms") {
            if (!need(i, a)) return false;
            o->gap_ms = std::atoi(argv[++i]);
        } else if (a == "--samples") {
            if (!need(i, a)) return false;
            o->samples = std::atoi(argv[++i]);
            if (o->samples < 2) o->samples = 2;
        } else if (a == "--wait-ms") {
            if (!need(i, a)) return false;
            o->wait_ms = std::atoi(argv[++i]);
        } else {
            *err = "unknown option: " + a;
            return false;
        }
    }
    if (!o->image.empty() && o->action == Action::Load) {
        *err = "--image and --no-flash are mutually exclusive (either flash or take it as loaded)";
        return false;
    }
    if (o->reboot && o->no_reboot) {
        *err = "--reboot and --no-reboot are mutually exclusive";
        return false;
    }
    if (o->gap_ms < 100) o->gap_ms = 100;
    if (o->settle_ms < 0) o->settle_ms = 0;
    if (o->quiet_timeout_ms < 1000) o->quiet_timeout_ms = 1000;
    if (o->wait_ms < 1000) o->wait_ms = 1000;
    if (o->md1_timeout_ms < 1000) o->md1_timeout_ms = 1000;
    if (o->mode_timeout_ms < 5000) o->mode_timeout_ms = 5000;
    if (o->beat_ms < 200) o->beat_ms = 200;
    if (o->mark_timeout_ms < 0) o->mark_timeout_ms = 0;
    return true;
}

void print_json_summary(const Options& o, int rc) {
    const char* act =
        o.action == Action::Check        ? "check"
        : o.action == Action::Load       ? "load"
        : o.action == Action::Flash      ? "flash"
        : o.action == Action::Restore    ? "restore"
        : o.action == Action::Status     ? "status"
        : o.action == Action::VerifyBoot ? "verify-boot"
        : o.action == Action::Activate   ? "activate"
        : o.action == Action::Resume     ? "resume"
        : o.action == Action::Deactivate ? "deactivate"
        : o.action == Action::Modes      ? "modes"
                                         : "selftest";
    std::string extra;
    if (!g_profile_used.empty()) extra = ",\"profile\":\"" + json_escape(g_profile_used) + "\"";
    std::printf("{\"tool\":\"%s\",\"action\":\"%s\",\"exit\":%d,\"state_file\":\"%s\"%s}\n",
                kToolVersion, act, rc,
                (o.state_path.empty() ? default_state_path() : o.state_path).c_str(),
                extra.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string err;
    if (!parse_args(argc, argv, &o, &err)) {
        log_error("%s", err.c_str());
        usage();
        return RC_USAGE;
    }
    g_quiet = o.quiet;

    if (o.action == Action::Help) {
        usage();
        return RC_OK;
    }

    int rc = RC_OK;
    switch (o.action) {
        case Action::Check: rc = action_check(o); break;
        case Action::Load: rc = action_load(o); break;
        case Action::Flash: rc = action_flash(o); break;
        case Action::Restore: rc = action_restore(o); break;
        case Action::Status: rc = action_status(o); break;
        case Action::VerifyBoot: rc = action_verify_boot(o); break;
        case Action::Activate: rc = action_activate(o); break;
        case Action::Resume: rc = action_resume(o); break;
        case Action::Deactivate: rc = action_deactivate(o); break;
        case Action::Modes: rc = action_modes(o); break;
        case Action::SelfTest: rc = action_selftest(o); break;
        case Action::Help: usage(); break;
    }
    if (o.json) print_json_summary(o, rc);
    return rc;
}
