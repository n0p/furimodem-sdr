// Selftest for the receive transport (CCB META DL ring).
//
//   ./selftest_transport                 # full suite (memory + file + driver)
//   ./selftest_transport --mkring FILE   # create an empty simulated ring
//   ./selftest_transport --peek          # look at the real node without consuming
//   ./selftest_transport --live          # consume the real node for a few seconds
//
// A laptop has no /dev/ccci_ccb_*, so the suite uses one ring simulated in
// memory and another in a file (same format as the phone's).
#include "SoapyFurimodemIQ.hpp"
#include "ccb_transport.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Formats.hpp>

#include <chrono>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace fmiq;

static int fails = 0;
static void expect(bool cond, const std::string& what) {
    std::printf("%-64s %s\n", what.c_str(), cond ? "OK" : "FAIL");
    if (!cond) ++fails;
}

// --------------------------------------------------------------- utilities

static void init_ctrl(volatile uint32_t* w, uint32_t page, uint32_t pages) {
    for (int i = 0; i < 16; ++i) w[i] = 0;
    w[0] = CcbRingReader::kGuardFirst;
    w[1] = 0;  // dl_alloc
    w[2] = 0;  // dl_free
    w[3] = 0;  // dl_read
    w[4] = 0;  // dl_write
    w[5] = page;
    w[6] = page * pages;
    w[7] = CcbRingReader::kGuardLast;
    w[8] = CcbRingReader::kGuardFirst;
    w[9] = 0;   // reserved (not used by the receive path)
    w[10] = 0;  // reserved
    w[11] = 0;  // reserved
    w[12] = 0;  // reserved
    w[13] = 0;  // reserved (not used by the receive path)
    w[14] = 0;
    w[15] = CcbRingReader::kGuardLast;
}

// Publishes a page the same way the kernel/modem does: first the payload with the
// page under construction, then magic+len, then the indices.
static bool publish_mem(volatile uint32_t* w, uint8_t* ring, uint32_t page, uint32_t pages,
                        const void* data, size_t n) {
    if (n + CcbRingReader::kPageHdr > page) return false;
    const uint32_t alloc = w[1];
    if (alloc >= pages) return false;
    const uint32_t nxt = (alloc + 1u) % pages;
    if (nxt == w[2]) return false;  // ring full
    uint8_t* p = ring + static_cast<size_t>(alloc) * page;
    auto* hdr = reinterpret_cast<volatile uint32_t*>(p);
    hdr[0] = CcbRingReader::kMagicBusy;
    hdr[1] = 0;
    std::memcpy(p + CcbRingReader::kPageHdr, data, n);
    hdr[1] = static_cast<uint32_t>(n);
    hdr[0] = CcbRingReader::kMagicValid;
    w[1] = nxt;  // dl_alloc
    w[4] = nxt;  // dl_write (publicada)
    return true;
}

// Producer on the ring simulated in a file (separate mapping from the consumer).
struct SimProducer {
    int fd = -1;
    void* base = nullptr;
    volatile uint32_t* w = nullptr;
    uint8_t* ring = nullptr;

    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0) return false;
        base = ::mmap(nullptr, simring::kFileBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) { base = nullptr; return false; }
        w = static_cast<volatile uint32_t*>(base) + simring::kEntry * 16;
        ring = static_cast<uint8_t*>(base) + simring::kRingOffset;
        return true;
    }
    bool publish(const void* d, size_t n) {
        return publish_mem(w, ring, simring::kPageSize, simring::kPages, d, n);
    }
    uint32_t writeIndex() const { return w[4]; }
    uint32_t readIndex() const { return w[3]; }
    uint32_t freeIndex() const { return w[2]; }
    ~SimProducer() {
        if (base) ::munmap(base, simring::kFileBytes);
        if (fd >= 0) ::close(fd);
    }
};

struct MemRing {
    void* map = nullptr;
    size_t len = 0;
    volatile uint32_t* w = nullptr;
    uint8_t* ring = nullptr;   // DL ring
    uint32_t page = 0, pages = 0;

    bool create(uint32_t pageSize, uint32_t nPages) {
        page = pageSize;
        pages = nPages;
        len = 4096 + static_cast<size_t>(pageSize) * nPages;
        map = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map == MAP_FAILED) { map = nullptr; return false; }
        w = static_cast<volatile uint32_t*>(map);
        ring = static_cast<uint8_t*>(map) + 4096;
        init_ctrl(w, page, pages);
        return true;
    }
    ~MemRing() { if (map) ::munmap(map, len); }
};

static inline uint8_t streamByte(size_t pos) { return static_cast<uint8_t>((pos * 7 + 3) & 0xff); }

static std::string make_tmp(const char* tag) {
    const char* dir = std::getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    std::string p = std::string(dir) + "/fmiq-" + std::string(tag) + "-XXXXXX";
    std::vector<char> buf(p.begin(), p.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    if (fd >= 0) ::close(fd);
    return std::string(buf.data());
}

// ------------------------------------------------- 1. reader over memory

static void test_ring_reader() {
    std::printf("\n== CcbRingReader (ring in memory) ==\n");
    MemRing r;
    if (!r.create(1024, 8)) { expect(false, "create ring in memory"); return; }
    CcbRingReader rd;
    CcbRingInfo info;
    info.entry = 0;
    info.dlPage = r.page;
    info.dlSize = r.page * r.pages;
    std::string err;
    expect(rd.attach(r.w, r.ring, info, &err), "attach with valid geometry");

    uint8_t out[8192];
    expect(rd.read(out, sizeof out) == 0, "empty ring -> 0 bytes");

    const char msg[] = "hello modem";
    expect(publish_mem(r.w, r.ring, r.page, r.pages, msg, sizeof msg), "publish 1 page");
    const int n1 = rd.read(out, sizeof out);
    expect(n1 == (int)sizeof msg && std::memcmp(out, msg, sizeof msg) == 0,
           "1 page -> payload intact");
    expect(rd.readIndex() == 1 && rd.freeIndex() == 1, "consuming advances dl_read and dl_free");

    // three pages of 1000 B in one large read
    uint8_t big[3000];
    for (int i = 0; i < 3000; ++i) big[i] = static_cast<uint8_t>(i * 7 + 1);
    publish_mem(r.w, r.ring, r.page, r.pages, big, 1000);
    publish_mem(r.w, r.ring, r.page, r.pages, big + 1000, 1000);
    publish_mem(r.w, r.ring, r.page, r.pages, big + 2000, 1000);
    const int n2 = rd.read(out, sizeof out);
    expect(n2 == 3000 && std::memcmp(out, big, 3000) == 0, "3 pages concatenated in order");
    expect(rd.readIndex() == 4 && rd.freeIndex() == 4, "dl_read/dl_free = 4 after 3 pages");

    // chunked read: 7 + 5 bytes of a 12-byte page
    const uint8_t chunk[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    publish_mem(r.w, r.ring, r.page, r.pages, chunk, sizeof chunk);
    uint8_t a[16] = {0}, b[16] = {0};
    const int na = rd.read(a, 7);
    const int nb = rd.read(b, 5);
    expect(na == 7 && std::memcmp(a, chunk, 7) == 0, "partial read (7 B)");
    expect(nb == 5 && std::memcmp(b, chunk + 7, 5) == 0, "rest of the page (5 B) with nothing lost");
    expect(rd.readIndex() == 5, "the page is released when the payload is exhausted, not before");

    // From here on the indices are handled by hand: rd = 5.
    auto plant = [&](uint32_t k, uint32_t magic, uint32_t len) {
        auto* h = reinterpret_cast<volatile uint32_t*>(r.ring + static_cast<size_t>(k) * r.page);
        h[0] = magic;
        h[1] = len;
    };

    // page under construction: it is seen as pending but neither consumed nor delivered
    plant(5, CcbRingReader::kMagicBusy, 4);
    r.w[4] = 6;  // dl_write ahead: the page is in progress
    expect(rd.read(out, sizeof out) == 0 && rd.readIndex() == 5,
           "magic 0x11111111 -> no data, no consumption");

    // corrupt page: discarded and skipped so the ring does not jam
    plant(5, 0xdeadbeef, 4);
    expect(rd.read(out, sizeof out) == 0 && rd.badPages() == 1, "invalid magic -> discarded and counted");
    expect(rd.readIndex() == 6, "corrupt page released (does not block the ring)");

    // impossible length
    plant(6, CcbRingReader::kMagicValid, r.page);  // > page-8
    r.w[4] = 7;
    expect(rd.read(out, sizeof out) == 0 && rd.badPages() == 2, "len beyond the page -> discarded");

    // corrupt indices: resynchronised to 0 instead of getting stuck
    r.w[4] = 999;
    const uint64_t dropped0 = rd.droppedPages();
    rd.read(out, sizeof out);
    expect(rd.droppedPages() > dropped0, "index out of range -> resync counted");
    expect(rd.readIndex() < r.pages, "after resync the indices are valid");

    // full ring wrap (the indices are modular, 0..n-1)
    MemRing r2;
    r2.create(512, 4);
    CcbRingReader rd2;
    CcbRingInfo i2;
    i2.entry = 0; i2.dlPage = r2.page; i2.dlSize = r2.page * r2.pages;
    rd2.attach(r2.w, r2.ring, i2, &err);
    uint8_t pat[4][500];
    for (int i = 0; i < 3; ++i) {
        std::memset(pat[i], 0x40 + i, sizeof pat[i]);
        publish_mem(r2.w, r2.ring, r2.page, r2.pages, pat[i], sizeof pat[i]);
    }
    uint8_t got[2000];
    int nw = rd2.read(got, sizeof got);
    bool okw = (nw == 1500);
    for (int i = 0; okw && i < 3; ++i) okw = std::memcmp(got + i * 500, pat[i], 500) == 0;
    expect(okw, "3 of 4 pages: correct order");
    expect(rd2.readIndex() == 3 && rd2.freeIndex() == 3, "cursor on the last page");
    // three more: dl_alloc and dl_write wrap to 0..2
    for (int i = 0; i < 3; ++i) publish_mem(r2.w, r2.ring, r2.page, r2.pages, pat[i], sizeof pat[i]);
    expect(r2.w[4] == 2, "write_index has wrapped (2)");
    nw = rd2.read(got, sizeof got);
    okw = (nw == 1500);
    for (int i = 0; okw && i < 3; ++i) okw = std::memcmp(got + i * 500, pat[i], 500) == 0;
    expect(okw, "ring wrap without losing order");
    expect(rd2.readIndex() == 2 && rd2.freeIndex() == 2, "indices wrapped correctly");
    // and one more page still works after the wrap
    publish_mem(r2.w, r2.ring, r2.page, r2.pages, pat[3], sizeof pat[3]);
    nw = rd2.read(got, sizeof got);
    expect(nw == 500 && std::memcmp(got, pat[3], 500) == 0, "read after the wrap");

    // real phone geometry
    MemRing r3;
    r3.create(simring::kPageSize, simring::kPages);
    CcbRingReader rd3;
    CcbRingInfo i3;
    i3.entry = 19; i3.dlPage = r3.page; i3.dlSize = r3.page * r3.pages;
    expect(rd3.attach(r3.w, r3.ring, i3, &err), "attach with META geometry (8 x 66560)");
    std::vector<uint8_t> payload(simring::kPageSize - 8);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 31 + 5);
    publish_mem(r3.w, r3.ring, r3.page, r3.pages, payload.data(), payload.size());
    std::vector<uint8_t> full(payload.size());
    expect(rd3.read(full.data(), full.size()) == (int)full.size() && full == payload,
           "maximum META page payload (66552 B) intact");

    // header with broken guards
    CcbRingReader bad;
    MemRing r4;
    r4.create(1024, 8);
    r4.w[15] = 0;
    expect(!bad.attach(r4.w, r4.ring, info, &err), "broken guard -> attach rejected");
}

// ------------------------------------------------- 1b. transport over a file

static void test_sim_transport() {
    std::printf("\n== CcbTransport over a ring simulated in a file ==\n");
    const std::string path = make_tmp("ring");
    std::string err;
    expect(simring::create(path, &err), "simring::create");
    SimProducer prod;
    expect(prod.open(path), "producer opens the ring");

    CcbOpenOptions o;
    o.simFile = path;
    auto tr = CcbTransport::open(o, &err);
    expect(tr != nullptr, "CcbTransport::open(sim)");
    if (!tr) { ::unlink(path.c_str()); return; }
    expect(tr->info().dlPages == simring::kPages && tr->info().dlPage == simring::kPageSize,
           "geometry read from the file");
    expect(tr->readIndex() == 0 && tr->writeIndex() == 0, "initial indices at zero");

    const uint8_t f1[64] = {1, 2, 3, 4, 5};
    expect(prod.publish(f1, 5), "publish 5 B into the simulated ring");
    uint8_t out[64] = {0};
    expect(tr->read(out, sizeof out) == 5 && std::memcmp(out, f1, 5) == 0, "read 5 B from the file");
    expect(prod.readIndex() == 1 && prod.freeIndex() == 1, "ack visible to the producer");

    // two pages at once and close/reopen: the indices persist
    const uint8_t f2[32] = {9, 8, 7, 6};
    prod.publish(f2, 10);
    prod.publish(f2, 3);
    tr.reset();
    auto tr2 = CcbTransport::open(o, &err);
    expect(tr2 != nullptr && tr2->pendingPages() == 2, "reopen: 2 pending pages");
    const int n = tr2 ? tr2->read(out, sizeof out) : -1;
    expect(n == 13 && out[0] == 9 && out[10] == 9, "13 B from two pages after reopening");
    tr2.reset();
    ::unlink(path.c_str());
}

// ------------------------------------- 2b. write over a simulated file

static void test_stress() {
    std::printf("\n== stress: concurrent producer and consumer ==\n");
    const std::string path = make_tmp("stress");
    std::string err;
    simring::create(path, &err);
    SimProducer prod;
    prod.open(path);
    CcbOpenOptions o;
    o.simFile = path;
    auto tr = CcbTransport::open(o, &err);
    if (!tr) { expect(false, "open transport for the stress test"); ::unlink(path.c_str()); return; }

    const size_t kPages = 300;  // ~750 KB: more than one lap of the 532 KB ring
    std::vector<uint16_t> lens(kPages);
    size_t total = 0;
    for (size_t i = 0; i < kPages; ++i) {
        lens[i] = static_cast<uint16_t>(1 + (i * 131) % 5000);
        total += lens[i];
    }

    std::atomic<bool> done{false};
    std::thread th([&] {
        std::vector<uint8_t> page(5000);
        size_t pos = 0;
        for (size_t i = 0; i < kPages; ++i) {
            const size_t n = lens[i];
            for (size_t k = 0; k < n; ++k) page[k] = streamByte(pos + k);
            while (!prod.publish(page.data(), n))
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            pos += n;
        }
        done = true;
    });

    std::vector<uint8_t> buf(4096);
    size_t got = 0;
    bool ok = true;
    const auto t0 = std::chrono::steady_clock::now();
    while (got < total) {
        const size_t want = 4 + (got % 1000);  // irregular sizes, like a real client
        const int n = tr->read(buf.data(), want);
        if (n > 0) {
            for (int k = 0; k < n; ++k)
                if (buf[k] != streamByte(got + k)) ok = false;
            got += static_cast<size_t>(n);
            continue;
        }
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if ((done && el > 0.2) || el > 5.0) break;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    th.join();
    std::printf("  %zu B expected, %zu read, %llu pages dropped\n", total, got,
                (unsigned long long)tr->droppedPages());
    expect(ok && got == total, "300 concurrent pages with no loss, duplication or reordering");
    tr.reset();
    ::unlink(path.c_str());
}

// ------------------------------------------------- 4. driver SoapySDR (RX)
static void test_driver() {
    std::printf("\n== SoapyFurimodemIQ::readStream with a simulated ring ==\n");
    const std::string path = make_tmp("drv");
    std::string err;
    simring::create(path, &err);
    SimProducer prod;
    prod.open(path);

    SoapySDR::Kwargs args;
    args["driver"] = "furimodem_iq";
    args["ring_file"] = path;
    SoapyFurimodemIQ dev(args);
    expect(dev.getHardwareInfo()["rx_transport"] == "none", "no transport before setupStream");
    auto* rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, SoapySDR::Kwargs());
    expect(dev.getHardwareInfo()["rx_transport"] == "sim", "setupStream(RX) opens the simulated ring");
    expect(dev.activateStream(rx, 0, 0, 0) == 0, "activateStream(RX)");

    std::vector<float> buf(4096);
    void* buffs[1] = {buf.data()};
    int flags = 0;
    long long ts = 0;
    expect(dev.readStream(rx, buffs, 1024, flags, ts, 5000) == SOAPY_SDR_TIMEOUT,
           "no data -> SOAPY_SDR_TIMEOUT");

    // known CS16 payload: 0, 0.5, -0.5, 32767/32768, -1, 1/32768
    const int16_t cs16[6] = {0, 16384, -16384, 32767, -32768, 1};
    prod.publish(cs16, sizeof cs16);
    const int got = dev.readStream(rx, buffs, 1024, flags, ts, 200000);
    expect(got == 3, "6 I/Q values = 3 complex samples");
    expect(got == 3 && buf[0] == 0.0f && buf[1] == 0.5f && buf[2] == -0.5f &&
               buf[3] == 32767.0f / 32768.0f && buf[4] == -1.0f && buf[5] == 1.0f / 32768.0f,
           "CS16 -> CF32 with scale 1/32768");
    expect(dev.readSensor("RX_PENDING") == "0", "RX_PENDING sensor at 0 after consuming");

    // block larger than the internal cap (1 MiB): it is split, no overflow
    std::vector<uint8_t> bigPage(66552, 0x11);
    prod.publish(bigPage.data(), bigPage.size());
    prod.publish(bigPage.data(), bigPage.size());
    std::vector<float> big(600000);
    void* bigb[1] = {big.data()};
    const int nb = dev.readStream(rx, bigb, 300000, flags, ts, 200000);
    expect(nb == 33276, "request of 300k samples -> both pages are served (33276)");

    // native CS16 format: the bytes arrive unchanged
    dev.deactivateStream(rx, 0, 0);
    dev.closeStream(rx);
    auto* rx16 = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {0}, SoapySDR::Kwargs());
    dev.activateStream(rx16, 0, 0, 0);
    const int16_t cs16b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    prod.publish(cs16b, sizeof cs16b);
    int16_t raw[64] = {0};
    void* buffs16[1] = {raw};
    const int got16 = dev.readStream(rx16, buffs16, 64, flags, ts, 200000);
    expect(got16 == 4 && std::memcmp(raw, cs16b, sizeof cs16b) == 0,
           "CS16 stream without conversion (4 samples)");

    // odd-length payload: the remaining bytes carry over to the next
    // read (23 + 23 = 46 B -> 11 samples and 2 B of tail)
    dev.deactivateStream(rx16, 0, 0);
    dev.closeStream(rx16);
    auto* rx2 = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, SoapySDR::Kwargs());
    dev.activateStream(rx2, 0, 0, 0);
    std::vector<int16_t> seq(23);  // 23 values = 46 B per page
    for (int i = 0; i < 23; ++i) seq[i] = static_cast<int16_t>(i * 100);
    prod.publish(seq.data(), 23 * 2);
    prod.publish(seq.data(), 23 * 2);
    std::vector<int16_t> acc;
    for (int k = 0; k < 8; ++k) {
        std::vector<float> b(16);
        void* bs[1] = {b.data()};
        const int n = dev.readStream(rx2, bs, 4, flags, ts, 200000);  // 4 samples per call
        if (n <= 0) break;
        for (int i = 0; i < n * 2; ++i) acc.push_back(static_cast<int16_t>(b[i] * 32768.0f));
    }
    // 92 B = 46 I/Q values = 23 samples, spread over 6 calls of 4
    bool seqOk = acc.size() == 46;
    for (size_t i = 0; seqOk && i < acc.size(); ++i)
        seqOk = acc[i] == seq[i % 23];
    expect(seqOk, "odd payload: 23 samples rebuilt without losing bytes");

    // without node or file: the driver keeps working and returns timeout
    SoapySDR::Kwargs none;
    none["driver"] = "furimodem_iq";
    none["ccb_node"] = "/nonexistent/ccci_ccb_meta";
    none["ccb_ctrl"] = "/nonexistent/ccci_ccb_ctrl";
    SoapyFurimodemIQ dev2(none);
    auto* rx3 = dev2.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, SoapySDR::Kwargs());
    dev2.activateStream(rx3, 0, 0, 0);
    expect(dev2.getHardwareInfo()["rx_transport"] == "none", "no node -> rx_transport=none");
    float dummy[16];
    void* bs[1] = {dummy};
    expect(dev2.readStream(rx3, bs, 8, flags, ts, 1000) == SOAPY_SDR_TIMEOUT,
           "no transport -> TIMEOUT (does not invent data)");

    dev.deactivateStream(rx2, 0, 0);
    dev.closeStream(rx2);
    ::unlink(path.c_str());
}

// ------------------------------------------------------------ 4. real node

static CcbOpenOptions live_opts(const char* node) {
    CcbOpenOptions o;
    if (node && *node) o.node = node;
    return o;
}

static int run_peek(const char* node) {
    std::string err;
    auto tr = CcbTransport::open(live_opts(node), &err);
    if (!tr) {
        std::printf("cannot open the ring: %s\n", err.c_str());
        return 2;
    }
    std::printf("transport  : %s\n", tr->description().c_str());
    std::printf("indices    : read=%u free=%u write=%u pending=%u\n",
                tr->readIndex(), tr->freeIndex(), tr->writeIndex(), tr->pendingPages());
    if (tr->readIndex() >= tr->info().dlPages || tr->writeIndex() >= tr->info().dlPages)
        std::printf("WARNING: indices outside 0..%u (free-running counters instead of modular?)\n",
                    tr->info().dlPages - 1);
    for (uint32_t k = 0; k < tr->pendingPages(); ++k) {
        const uint32_t i = (tr->readIndex() + k) % tr->info().dlPages;
        uint32_t magic = 0, len = 0;
        tr->peek(i, magic, len);
        std::printf("  page %u: magic=0x%08x len=%u\n", i, magic, len);
    }
    std::printf("(peek does not consume: it does not touch dl_read/dl_free)\n");
    return 0;
}

static int run_live(const char* node, double seconds) {
    std::string err;
    auto tr = CcbTransport::open(live_opts(node), &err);
    if (!tr) {
        std::printf("cannot open the ring: %s\n", err.c_str());
        return 2;
    }
    std::printf("transport  : %s\n", tr->description().c_str());
    std::vector<uint8_t> buf(1 << 20);
    uint64_t total = 0, pages = 0;
    bool dumped = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        const uint32_t before = tr->readIndex();
        const int n = tr->read(buf.data(), buf.size());
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (!dumped) {
            std::printf("first read: %d B\n  hex:", n);
            for (int i = 0; i < n && i < 64; ++i) std::printf(" %02x", buf[i]);
            std::printf("\n");
            dumped = true;
        }
        total += static_cast<uint64_t>(n);
        pages += (tr->readIndex() >= before) ? (tr->readIndex() - before)
                                             : (tr->info().dlPages - before + tr->readIndex());
    }
    std::printf("read %llu B from the META DL ring in %.1f s (%.1f B/s, %llu pages)\n",
                (unsigned long long)total, seconds,
                total / (seconds > 0 ? seconds : 1.0), (unsigned long long)pages);
    std::printf("dropped=%llu bad=%llu\n",
                (unsigned long long)tr->droppedPages(), (unsigned long long)tr->badPages());
    return total ? 0 : 3;
}

static int run_device(double seconds) {
    SoapySDR::Kwargs a;
    a["driver"] = "furimodem_iq";
    SoapyFurimodemIQ dev(a);
    auto* rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, SoapySDR::Kwargs());
    dev.activateStream(rx, 0, 0, 0);
    std::printf("rx_transport : %s\n", dev.getHardwareInfo()["rx_transport"].c_str());
    std::vector<float> buf(8192);
    void* b[1] = {buf.data()};
    int flags = 0;
    long long ts = 0;
    uint64_t samples = 0, calls = 0, timeouts = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        const int n = dev.readStream(rx, b, 4096, flags, ts, 200000);
        if (n > 0) {
            if (samples == 0) {
                std::printf("first block: %d samples; I/Q[0..3] =", n);
                for (int i = 0; i < 8 && i < n * 2; ++i) std::printf(" %.4f", buf[i]);
                std::printf("\n");
            }
            samples += static_cast<uint64_t>(n);
            ++calls;
        } else if (n == SOAPY_SDR_TIMEOUT) {
            ++timeouts;
        } else {
            std::printf("readStream returned %d\n", n);
            break;
        }
    }
    std::printf("readStream: %llu samples in %llu blocks (%.1f kSa/s), %llu timeouts\n",
                (unsigned long long)samples, (unsigned long long)calls,
                samples / (seconds > 0 ? seconds : 1.0) / 1000.0, (unsigned long long)timeouts);
    std::printf("sensors: RX_PENDING=%s RX_DROPPED=%s RX_BAD=%s\n",
                dev.readSensor("RX_PENDING").c_str(), dev.readSensor("RX_DROPPED").c_str(),
                dev.readSensor("RX_BAD").c_str());
    dev.deactivateStream(rx, 0, 0);
    dev.closeStream(rx);
    if (!samples)
        std::printf("no data: check permissions on /dev/ccci_ccb_* (sudo) and use --peek\n");
    return samples ? 0 : 3;
}

static void usage(const char* argv0);

static void usage(const char* argv0) {
    std::printf(
        "usage: %s [--selftest | --mkring FILE | --peek [NODE] | --live [NODE] [SEC] | --device [SEC]]\n"
        "  (with no arguments it runs the full suite: memory + file + driver)\n",
        argv0);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string a = argv[1];
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--mkring") {
            if (argc < 3) { usage(argv[0]); return 2; }
            std::string err;
            if (!simring::create(argv[2], &err)) {
                std::printf("error: %s\n", err.c_str());
                return 2;
            }
            std::printf("simulated ring created: %s (%zu B)\n", argv[2], simring::kFileBytes);
            return 0;
        }
        if (a == "--peek") return run_peek(argc > 2 ? argv[2] : nullptr);
        if (a == "--live") {
            const double secs = argc > 3 ? std::atof(argv[3]) : 3.0;
            return run_live(argc > 2 ? argv[2] : nullptr, secs);
        }
        if (a == "--device") return run_device(argc > 2 ? std::atof(argv[2]) : 3.0);
        if (a != "--selftest") { usage(argv[0]); return 2; }
    }

    // 1) temporary files go to TMPDIR (or /tmp if it is not set)
    test_ring_reader();
    test_sim_transport();
    test_stress();
    test_driver();

    std::printf("\n%s (%d failure(s))\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
