// send_ccb — minimal sender over the CCB UL ring (AP -> MD).
//
// It only uses ccb_transport: no SoapySDR, no driver and no state mirror. It publishes N
// pages on the UL ring of the CCB user (META by default), rings once per message
// (unless --no-notify) and waits for the modem ack (ul_free).
//
// Builds on the phone as is, with no external dependencies:
//   g++ -O2 -std=c++17 -pthread send_ccb.cpp ccb_transport.cpp -o send_ccb
//
// The state counters (+0x40304 CAPTURES, +0x40340 MESSAGES, +0x40310 heartbeat) are
// NOT read here: they are read with a separate tool before and after. This binary
// only publishes and reports what happened with the indices.
//
// Exit codes:
//   0  all messages published and consumed by the modem (in_flight = 0)
//   2  bad arguments or node open failure
//   3  published, but the modem did not consume everything within --wait-ms
//   4  not all messages could be published (ring full / payload does not fit)

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ccb_transport.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void sleep_ms(double ms) {
    if (ms <= 0) return;
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(ms * 1000.0)));
}

struct Args {
    std::string node = "/dev/ccci_ccb_meta";
    std::string ctrl = "/dev/ccci_ccb_ctrl";
    std::string sim;  // if not empty: ring simulated in a file (tests without the phone)
    int entry = -1;
    int count = 1;
    double gapMs = 0.0;
    double waitMs = 500.0;
    double fullMs = 2000.0;   // how long to wait for the modem to free pages when the ring is full
    uint32_t notifyCore = 0;
    bool notify = true;
    bool verbose = false;
    std::string hex = "01000000";
    std::string text;
    bool haveText = false;
};

void usage(const char* argv0) {
    std::printf(
        "usage: %s [--count N] [--gap-ms MS] [--send HEX | --text TEXT]\n"
        "       [--wait-ms MS] [--no-notify] [--notify-core N] [--entry N]\n"
        "       [--node NODE] [--ctrl NODE] [--sim FILE] [--verbose]\n"
        "\n"
        "  Publishes N messages (one page each) on the CCB UL ring and rings the doorbell.\n"
        "  HEX accepts spaces (\"41 54 2b\"). Default 01000000 (4 B).\n"
        "  --gap-ms separates messages; 0 = burst. --wait-ms waits for the ack (ul_free).\n"
        "\n"
        "example: %s --send 41542b43474d490d --count 10 --gap-ms 50 --wait-ms 500\n",
        argv0, argv0);
}

bool parse_hex(const std::string& in, std::vector<uint8_t>& out, std::string* err) {
    std::string h;
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == ':') continue;
        h.push_back(c);
    }
    if (h.rfind("0x", 0) == 0) h.erase(0, 2);
    if (h.rfind("0X", 0) == 0) h.erase(0, 2);
    if (h.empty() || (h.size() % 2) != 0) {
        *err = "invalid hex (digit pairs required): " + in;
        return false;
    }
    for (size_t i = 0; i < h.size(); i += 2) {
        unsigned v = 0;
        if (std::sscanf(h.c_str() + i, "%2x", &v) != 1) {
            *err = "invalid hex at byte " + std::to_string(i / 2);
            return false;
        }
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

// Printable form for the summary; the rest is shown in hex.
std::string printable(const std::vector<uint8_t>& p) {
    std::string s;
    for (uint8_t b : p) {
        if (b >= 0x20 && b < 0x7f) {
            s.push_back(static_cast<char>(b));
        } else if (b == '\r') {
            s += "\\r";
        } else if (b == '\n') {
            s += "\\n";
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", b);
            s += buf;
        }
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::printf("missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (opt == "-h" || opt == "--help") {
            usage(argv[0]);
            return 0;
        } else if (opt == "--count" || opt == "--repeat") {
            a.count = std::atoi(next(opt.c_str()));
        } else if (opt == "--gap-ms") {
            a.gapMs = std::atof(next(opt.c_str()));
        } else if (opt == "--wait-ms") {
            a.waitMs = std::atof(next(opt.c_str()));
        } else if (opt == "--full-ms") {
            a.fullMs = std::atof(next(opt.c_str()));
        } else if (opt == "--send" || opt == "--payload") {
            a.hex = next(opt.c_str());
            a.haveText = false;
        } else if (opt == "--text") {
            a.text = next(opt.c_str());
            a.haveText = true;
        } else if (opt == "--no-notify") {
            a.notify = false;
        } else if (opt == "--notify-core") {
            a.notifyCore = static_cast<uint32_t>(std::strtoul(next(opt.c_str()), nullptr, 0));
        } else if (opt == "--entry") {
            a.entry = std::atoi(next(opt.c_str()));
        } else if (opt == "--node") {
            a.node = next(opt.c_str());
        } else if (opt == "--ctrl") {
            a.ctrl = next(opt.c_str());
        } else if (opt == "--sim") {
            a.sim = next(opt.c_str());
        } else if (opt == "--verbose" || opt == "-v") {
            a.verbose = true;
        } else {
            std::printf("unknown option: %s\n\n", opt.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (a.count < 0 || a.count > 100000) {
        std::printf("--count out of range (0..100000)\n");
        return 2;
    }

    std::vector<uint8_t> payload;
    if (a.haveText) {
        payload.assign(a.text.begin(), a.text.end());
    } else {
        std::string err;
        if (!parse_hex(a.hex, payload, &err)) {
            std::printf("%s\n", err.c_str());
            return 2;
        }
    }
    if (payload.empty()) {
        std::printf("empty payload: use --send HEX or --text TEXT\n");
        return 2;
    }

    fmiq::CcbOpenOptions o;
    o.node = a.node;
    o.ctrl = a.ctrl;
    o.entryHint = a.entry;
    o.writableRing = true;
    o.notifyCore = a.notifyCore;
    o.simFile = a.sim;
    std::string err;
    auto tr = fmiq::CcbTransport::open(o, &err);
    if (!tr) {
        std::printf("error      : cannot open the ring: %s\n", err.c_str());
        return 2;
    }
    if (!tr->tx()) {
        std::printf("error      : the transport opened WITHOUT a writer (writableRing ignored?)\n");
        return 2;
    }
    const fmiq::CcbRingWriter& w = tr->writer();

    std::printf("transport  : %s\n", tr->description().c_str());
    std::printf("payload    : %zu B \"%s\" x %d  gap %.1f ms  doorbell %s (core %u)\n",
                payload.size(), printable(payload).c_str(), a.count, a.gapMs,
                a.notify ? "yes" : "NO", a.notifyCore);
    if (payload.size() > w.maxPayload()) {
        std::printf("error      : the payload does not fit in one page (max %u B)\n", w.maxPayload());
        return 2;
    }
    std::printf("before     : ul_alloc=%u ul_write=%u ul_read=%u ul_free=%u free=%u in_flight=%u\n",
                w.allocIndex(), w.writeIndex(), w.readIndex(), w.freeIndex(), w.freePages(),
                w.inFlightPages());

    // --- publication -------------------------------------------------------
    uint64_t posted = 0, stalls = 0, bellFails = 0;
    const auto t0 = Clock::now();
    for (int i = 0; i < a.count; ++i) {
        const uint32_t page = w.allocIndex();
        int r = 0;
        for (;;) {
            r = tr->post(payload.data(), payload.size());
            if (r != 0) break;
            ++stalls;
            if (ms_since(t0) > a.fullMs) break;
            sleep_ms(0.2);  // ring full: the modem is draining
        }
        if (r <= 0) {
            std::printf("message %d rejected (r=%d)%s\n", i + 1, r,
                        r == 0 ? ": ring full, the modem is not consuming" : ": does not fit in one page");
            break;
        }
        ++posted;
        bool rang = false;
        if (a.notify) {
            rang = tr->flush();
            if (!rang) ++bellFails;
        }
        if (a.verbose) {
            uint32_t magic = 0, len = 0;
            w.peek(page, magic, len);
            std::printf("  #%d page=%u magic=%08x len=%u doorbell=%s\n", i + 1, page, magic, len,
                        a.notify ? (rang ? "yes" : "FAIL") : "-");
        }
        if (a.gapMs > 0 && i + 1 < a.count) sleep_ms(a.gapMs);
    }
    const double sendMs = ms_since(t0);
    if (bellFails)
        std::printf("WARNING    : %llu ioctl SMEM_TX_NOTIFY calls failed (pages published unnotified)\n",
                    (unsigned long long)bellFails);

    // --- modem ack (ul_free reaches ul_write) ------------------------------
    const auto t1 = Clock::now();
    if (a.notify && posted > 0) {
        while (w.inFlightPages() > 0 && ms_since(t1) < a.waitMs) sleep_ms(0.1);
    }
    const double drainMs = ms_since(t1);

    std::printf("after      : ul_alloc=%u ul_write=%u ul_read=%u ul_free=%u free=%u in_flight=%u\n",
                w.allocIndex(), w.writeIndex(), w.readIndex(), w.freeIndex(), w.freePages(),
                w.inFlightPages());
    std::printf("published  : %llu/%d  full_rejects=%llu  doorbells=%llu  resyncs=%llu  %.1f ms\n",
                (unsigned long long)posted, a.count, (unsigned long long)stalls,
                (unsigned long long)w.doorbells(), (unsigned long long)w.resyncs(), sendMs);
    if (posted == 0) {
        std::printf("drain      : nothing published\n");
        std::printf("RESULT     : FAIL posted=0/%d\n", a.count);
        return 4;
    }
    if (w.inFlightPages() == 0) {
        std::printf("drain      : the modem drained everything in %.2f ms (ul_free reached ul_write)\n", drainMs);
        std::printf("RESULT     : OK posted=%llu/%d drained=1\n", (unsigned long long)posted, a.count);
        return posted == static_cast<uint64_t>(a.count) ? 0 : 4;
    }
    std::printf("drain      : %u page(s) unconsumed after %.0f ms (modem stopped?)\n",
                w.inFlightPages(), a.waitMs);
    std::printf("RESULT     : NO_ACK posted=%llu/%d drained=0 in_flight=%u\n",
                (unsigned long long)posted, a.count, w.inFlightPages());
    return posted == static_cast<uint64_t>(a.count) ? 3 : 4;
}
