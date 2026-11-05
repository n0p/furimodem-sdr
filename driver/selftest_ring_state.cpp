// Tests for the state the module reports through readSetting("rx_ring").
//
// The app used to see the same SOAPY_SDR_TIMEOUT for "the window is not there", "it is mapped but
// the firmware publishes nothing" and "the blocks use a signature I do not know". This exercises
// the three cases on a real SoapyFurimodemIQ object, with the ring simulated by a regular file
// (IqWindow falls back to the file size when the node does not answer the window ioctl).
#include "SoapyFurimodemIQ.hpp"
#include "iq_window.hpp"

#include <SoapySDR/Device.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-62s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// A block with a known signature (CSD1) and a payload, so it counts as "valid".
void write_block(int fd, const fmiq::IqRingLayout& l, std::size_t idx, std::uint32_t magic,
                 std::uint32_t seq) {
    std::vector<std::uint8_t> b(l.block_bytes, 0x58);  // payload with a valid 5-bit pattern
    const std::uint32_t bytes = std::uint32_t(l.block_bytes - l.header_bytes);
    std::memcpy(b.data() + 0x00, &magic, 4);
    std::memcpy(b.data() + 0x04, &seq, 4);
    std::memcpy(b.data() + 0x0C, &bytes, 4);
    const std::uint32_t zero = 0;
    std::memcpy(b.data() + 0x10, &zero, 4);  // state: finished
    const off_t off = off_t(l.window_off + idx * l.block_bytes);
    if (::pwrite(fd, b.data(), b.size(), off) != off_t(b.size()))
        std::printf("  (short write at 0x%llx)\n", static_cast<unsigned long long>(off));
}

std::string make_ring_file(const fmiq::IqRingLayout& l, const char* path) {
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return std::string();
    const off_t size = off_t(l.window_off + l.blocks * l.block_bytes);
    if (::ftruncate(fd, size) != 0) {
        ::close(fd);
        return std::string();
    }
    return std::string(path);
}

// The module with the ring in that file and the CCB transport off: readSetting must work without
// touching any device node.
struct Device {
    SoapySDR::Kwargs args;
    std::unique_ptr<fmiq::SoapyFurimodemIQ> dev;
    SoapySDR::Stream* stream = nullptr;

    explicit Device(const std::string& node) {
        args["iq_node"] = node;
        args["transport"] = "0";
        dev.reset(new fmiq::SoapyFurimodemIQ(args));
    }
    void openRx() { stream = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, {}); }
    ~Device() {
        if (stream) dev->closeStream(stream);
    }
};

}  // namespace

int main() {
    const fmiq::IqRingLayout l;
    std::printf("ring layout: off=0x%zx blocks=%zu block=0x%zx\n", l.window_off, l.blocks, l.block_bytes);

    // 1. Nothing has been opened yet.
    {
        Device d("/nonexistent/iq-node");
        const std::string v = d.dev->readSetting("rx_ring");
        std::printf("  before setupStream: %s\n", v.c_str());
        check(v == "not-opened", "rx_ring before setupStream is not-opened");
    }

    // 2. The node does not exist: not mapped, and it says why.
    {
        Device d("/nonexistent/iq-node");
        d.openRx();
        const std::string v = d.dev->readSetting("rx_ring");
        std::printf("  missing node: %s\n", v.c_str());
        check(starts_with(v, "no-device"), "a missing window is no-device");
        check(v.find("/nonexistent/iq-node") != std::string::npos, "the detail names the node");
    }

    // 3. Mapped but nothing published: the firmware is not capturing (not a format problem).
    const char* tmpdir = std::getenv("TMPDIR");
    const std::string path = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/fmiq-ring-selftest.bin";
    {
        const std::string made = make_ring_file(l, path.c_str());
        check(!made.empty(), "simulated ring file created");
        Device d(made);
        d.openRx();
        const std::string v = d.dev->readSetting("rx_ring");
        std::printf("  empty ring: %s\n", v.c_str());
        check(v == "no-blocks", "a mapped ring without blocks is no-blocks");

        // 4. A published capture with a known signature: the ring is working.
        const int fd = ::open(path.c_str(), O_RDWR);
        check(fd >= 0, "ring file reopened for writing");
        if (fd >= 0) {
            write_block(fd, l, 3, l.magic_packed, 7);
            ::close(fd);
            const std::string ok = d.dev->readSetting("rx_ring");
            std::printf("  one published block: %s\n", ok.c_str());
            check(ok == "ok", "a valid block is ok");

            // 5. The same slot with a signature this driver does not know: format mismatch.
            const int fd2 = ::open(path.c_str(), O_RDWR);
            if (fd2 >= 0) {
                write_block(fd2, l, 3, 0x42424242u, 8);
                ::close(fd2);
                const std::string bad = d.dev->readSetting("rx_ring");
                std::printf("  unknown signature: %s\n", bad.c_str());
                check(starts_with(bad, "no-format"), "an unknown signature is no-format");
                check(bad.find("0x42424242") != std::string::npos, "the detail carries the signature");
            }
        }
        ::unlink(path.c_str());
    }

    if (failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", failures);
    return 1;
}
