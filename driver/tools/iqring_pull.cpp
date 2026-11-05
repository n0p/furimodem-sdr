// IQ ring reader on the phone, using the SAME class (IqWindow) as the Soapy driver.
// It verifies that the firmware leaves the factory dump IQ in the ring without losing blocks:
// it polls the ring, keeps every new block and writes it to a file preceded by its sequence
// number, so the capture can be reassembled elsewhere.
//
// Usage (root, on the phone):
//     ./iqring_pull 3 /tmp/ring.blocks          # 3 s of polling (drains in order)
//     ./iqring_pull 3 /tmp/ring.blocks /dev/ccci_raw_mdm
//     ./iqring_pull 3 /tmp/ring.blocks /dev/ccci_raw_mdm newest   # only the latest
//
// File format: the block sequence as is (0x80 B header with the format signature,
// "CSD1"/"CSD8"/"RF30", and its payload). It is the same thing tools/make-iqfile.py generates and
// what the app reads with --source file, so the phone dump opens without conversion.
#include "iq_format.hpp"
#include "iq_window.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <time.h>
#include <vector>

namespace {

double now_s() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// 0x80 B header with the same fields the ring publishes. The signature keeps the format of the
// block that was read ("CSD1"/"CSD8"/"RF30").
void write_header(std::uint8_t* h, const fmiq::IqBlockInfo& b, std::uint32_t bytes) {
    std::memset(h, 0, fmiq::kBlockHeaderBytes);
    std::uint32_t sig = fmiq::kMagicCsd1;
    if (b.format == fmiq::IqSampleFormat::Packed8) sig = fmiq::kMagicCsd8;
    else if (b.format == fmiq::IqSampleFormat::Cs16) sig = fmiq::kMagicRf30;
    std::memcpy(h + 0x00, &sig, 4);
    std::memcpy(h + 0x04, &b.seq, 4);
    std::memcpy(h + 0x08, &b.ucnt, 4);
    std::memcpy(h + 0x0C, &bytes, 4);
    const std::uint32_t zero = 0;
    std::memcpy(h + 0x10, &zero, 4);  // state: finished
    std::memcpy(h + 0x14, &b.blk, 4);
    std::memcpy(h + 0x18, &b.push, 4);
    std::memcpy(h + 0x1C, &b.slot, 4);
    std::memcpy(h + 0x64, &b.p2, 4);
    std::memcpy(h + 0x68, &b.p3, 4);
    std::memcpy(h + 0x6C, &b.stale, 4);
}

}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 3.0;
    const std::string out = argc > 2 ? argv[2] : "/tmp/ring.blocks";
    const std::string node = argc > 3 ? argv[3] : "/dev/ccci_raw_mdm";
    const bool newest = argc > 4 && std::strcmp(argv[4], "newest") == 0;

    fmiq::IqRingLayout lay;  // 0x1D81000, 16 blocks, 16384 B, 128 B header, "RF30"
    fmiq::IqWindow w(lay);
    std::string err;
    if (!w.open(node, &err)) {
        std::fprintf(stderr, "cannot open: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\nwindow %s  mode %s\n", w.description().c_str(), node.c_str(),
                newest ? "read (most recent)" : "read_next (in order)");

    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        std::perror(out.c_str());
        return 1;
    }
    std::vector<std::uint8_t> buf(lay.block_bytes);
    std::uint8_t header[fmiq::kBlockHeaderBytes];
    const double t0 = now_s();
    double tprint = t0;
    std::size_t total = 0;
    unsigned long calls = 0, with_data = 0;

    while (now_s() - t0 < seconds) {
        const std::size_t n = newest ? w.read(buf.data(), buf.size())
                                     : w.read_next(buf.data(), buf.size());
        ++calls;
        if (n == 0) continue;
        ++with_data;
        total += n;
        const fmiq::IqBlockInfo& info = w.last_block();
        write_header(header, info, static_cast<std::uint32_t>(n));
        std::fwrite(header, 1, sizeof(header), f);
        std::fwrite(buf.data(), 1, n, f);
        const double t = now_s();
        if (t - tprint >= 0.5) {
            tprint = t;
            std::printf("  t %5.1f s  seq %8u  %5zu B  %s  %.3f MHz  read %u  lost %u\n",
                        t - t0, info.seq, n, fmiq::format_name(info.format),
                        double(info.freq_hz) / 1e6, w.blocks_read(), w.blocks_lost());
        }
    }
    std::fclose(f);
    std::printf("\ncalls %lu  with data %lu  B %zu\n", calls, with_data, total);
    std::printf("blocks read %u  lost %u  last seq %u\n",
                w.blocks_read(), w.blocks_lost(), w.last_seq());
    std::printf("saved to %s\n", out.c_str());
    return w.blocks_read() ? 0 : 2;
}
