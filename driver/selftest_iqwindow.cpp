// Tests for the IQ ring reader: a fake ring is built in memory and the selection of new
// blocks, wrap detection and degenerate cases are checked.
#include "iq_window.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-58s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
}

void write_block(std::uint8_t* ring, const fmiq::IqRingLayout& l, std::size_t idx, std::uint32_t seq,
                 std::uint32_t ucnt, std::uint32_t bytes, std::uint8_t fill) {
    std::uint8_t* b = ring + idx * l.block_bytes;
    std::memcpy(b + 0, &l.magic_legacy, 4);
    std::memcpy(b + 4, &seq, 4);
    std::memcpy(b + 8, &ucnt, 4);
    std::memcpy(b + 12, &bytes, 4);
    std::memset(b + l.header_bytes, fill, l.block_bytes - l.header_bytes);
}

// Packed block: "CSD1" (5-bit) or "CSD8" (8-bit), with the burst frequency in p3.
void write_packed_block(std::uint8_t* ring, const fmiq::IqRingLayout& l, std::size_t idx,
                        std::uint32_t magic, std::uint32_t seq, std::uint32_t bytes,
                        std::uint32_t p3, std::uint32_t stale = 0) {
    std::uint8_t* b = ring + idx * l.block_bytes;
    std::memcpy(b + 0, &magic, 4);
    std::memcpy(b + 4, &seq, 4);
    const std::uint32_t zero = 0;
    std::memcpy(b + 8, &zero, 4);   // ucnt
    std::memcpy(b + 12, &bytes, 4);
    std::memcpy(b + 16, &zero, 4);  // state: finished
    const std::uint32_t blk = 0, push = seq, slot = 0;
    std::memcpy(b + 0x14, &blk, 4);
    std::memcpy(b + 0x18, &push, 4);
    std::memcpy(b + 0x1C, &slot, 4);
    std::memcpy(b + 0x68, &p3, 4);
    std::memcpy(b + 0x6C, &stale, 4);
    // Fill with valid 5-bit words (bits 0-2 and 8-10 zero), which is what tells the
    // format apart by content in old dumps.
    for (std::size_t o = l.header_bytes; o + 1 < l.block_bytes; o += 2) {
        b[o] = 0x58;
        b[o + 1] = 0x58;
    }
}

}  // namespace

int main() {
    fmiq::IqRingLayout l;
    l.window_off = 0;
    l.blocks = 4;
    l.block_bytes = 1024;
    l.header_bytes = 128;

    std::vector<std::uint8_t> ring(l.blocks * l.block_bytes, 0);
    fmiq::IqWindow w(l);
    w.attach_for_test(ring.data(), ring.size());

    std::vector<std::uint8_t> dst(2048);

    check(w.read(dst.data(), dst.size()) == 0, "empty ring -> delivers nothing");

    write_block(ring.data(), l, 0, 1, 1000, 512, 0xA1);
    std::size_t n = w.read(dst.data(), dst.size());
    check(n == 512 && dst[0] == 0xA1, "first valid block -> 512 B with its fill");
    check(w.blocks_read() == 1 && w.blocks_lost() == 0, "first read does not count losses");
    check(w.read(dst.data(), dst.size()) == 0, "same sequence -> nothing new");

    write_block(ring.data(), l, 1, 2, 2000, 512, 0xB2);
    n = w.read(dst.data(), dst.size());
    check(n == 512 && dst[0] == 0xB2, "next block -> the new one is delivered");
    check(w.last_seq() == 2 && w.blocks_lost() == 0, "sequence advances without gaps");

    write_block(ring.data(), l, 2, 5, 5000, 512, 0xC3);
    n = w.read(dst.data(), dst.size());
    check(n == 512 && dst[0] == 0xC3, "sequence jump -> delivers the most recent");
    check(w.blocks_lost() == 2, "jump 2->5 counts two lost blocks");

    // Block with a corrupt signature: ignored.
    std::uint8_t* b3 = ring.data() + 3 * l.block_bytes;
    write_block(ring.data(), l, 3, 6, 6000, 512, 0xD4);
    const std::uint32_t bad = 0xDEADBEEF;
    std::memcpy(b3, &bad, 4);
    n = w.read(dst.data(), dst.size());
    check(n == 0, "block with corrupt signature is discarded");
    check(w.last_seq() == 5, "sequence does not advance on an invalid block");

    // Impossible length: discarded instead of reading out of bounds.
    write_block(ring.data(), l, 3, 7, 7000, 4000, 0xD5);
    check(w.read(dst.data(), dst.size()) == 0, "length larger than the block is discarded");

    // Ring wraps: the sequence keeps increasing even if the index repeats.
    write_block(ring.data(), l, 0, 8, 8000, 256, 0xE6);
    n = w.read(dst.data(), dst.size());
    check(n == 256 && dst[0] == 0xE6, "ring wrap -> delivered by sequence");

    // Small dst: delivered truncated, without overflowing.
    write_block(ring.data(), l, 1, 9, 9000, 512, 0xF7);
    std::vector<std::uint8_t> small(64);
    n = w.read(small.data(), small.size());
    check(n == 64 && small[0] == 0xF7, "small dst -> truncated copy without overflow");

    // Block being written (state != 0): ignored even if the sequence is higher.
    write_block(ring.data(), l, 2, 20, 20000, 512, 0x18);
    const std::uint32_t in_progress = 1;
    std::memcpy(ring.data() + 2 * l.block_bytes + 16, &in_progress, 4);
    check(w.read(dst.data(), dst.size()) == 0, "block in progress (state=1) is not delivered");

    // read_next delivers in order: with two new blocks in a row none is skipped.
    {
        fmiq::IqRingLayout l2;
        l2.window_off = 0;
        l2.blocks = 4;
        l2.block_bytes = 1024;
        l2.header_bytes = 128;
        std::vector<std::uint8_t> ring2(l2.blocks * l2.block_bytes, 0);
        fmiq::IqWindow w2(l2);
        w2.attach_for_test(ring2.data(), ring2.size());
        std::vector<std::uint8_t> d2(2048);
        write_block(ring2.data(), l2, 0, 10, 1, 256, 0x11);
        write_block(ring2.data(), l2, 1, 11, 1, 256, 0x22);
        write_block(ring2.data(), l2, 2, 12, 1, 256, 0x33);
        std::size_t m = w2.read_next(d2.data(), d2.size());
        check(m == 256 && d2[0] == 0x11, "read_next: the oldest first");
        m = w2.read_next(d2.data(), d2.size());
        check(m == 256 && d2[0] == 0x22, "read_next: then the next one (no skips)");
        m = w2.read_next(d2.data(), d2.size());
        check(m == 256 && d2[0] == 0x33, "read_next: and the third one");
        check(w2.blocks_lost() == 0 && w2.blocks_read() == 3, "read_next: 3 read, 0 lost");
        check(w2.read_next(d2.data(), d2.size()) == 0, "read_next: nothing new returns 0");
    }

    // Geometry that does not fit: attach must reject it.
    fmiq::IqWindow big(l);
    big.attach_for_test(ring.data(), 100);
    std::vector<std::uint8_t> tmp(64);
    check(big.read(tmp.data(), tmp.size()) == 0, "geometry larger than the memory -> no read");

    // --- new format: packed I/Q with the frequency in the header ---
    {
        std::vector<std::uint8_t> ring5(l.blocks * l.block_bytes, 0);
        fmiq::IqWindow w5(l);
        w5.attach_for_test(ring5.data(), ring5.size());
        // p3 high 16 = 21150 -> 2115.0 MHz; payload of 1024 B (512 samples).
        write_packed_block(ring5.data(), l, 0, l.magic_packed, 1, 512, 21150u << 16, 0);
        fmiq::IqBlockInfo info;
        std::size_t n = w5.read(dst.data(), dst.size(), &info);
        check(n == 512 && dst[0] == 0x58, "CSD1 -> delivers the block payload");
        check(info.format == fmiq::IqSampleFormat::Packed5 && info.sample_bytes == 2,
              "CSD1 -> 5-bit format, 2 B per sample");
        check(info.freq_hz == 2115000000ull, "CSD1 -> p3 decodes to 2115.0 MHz");
        check(info.stale == 0 && info.seq == 1, "CSD1 -> seq and stale marker");
        check(w5.last_block().freq_hz == 2115000000ull, "CSD1 -> last_block() keeps the frequency");

        // The "CSD8" signature is the one that declares 8 bits: no need to look at the payload.
        write_packed_block(ring5.data(), l, 1, l.magic_packed8, 2, 512, 21150u << 16, 3);
        n = w5.read(dst.data(), dst.size(), &info);
        check(n == 512 && info.format == fmiq::IqSampleFormat::Packed8 && info.sample_bytes == 2,
              "CSD8 -> 8-bit format, 2 B per sample");
        check(info.freq_hz == 2115000000ull && info.stale == 3 && info.seq == 2,
              "CSD8 -> frequency, seq and stale from the header");

        // An out-of-range p3 does not invent a frequency.
        write_packed_block(ring5.data(), l, 2, l.magic_packed, 3, 512, 7u << 16);
        n = w5.read(dst.data(), dst.size(), &info);
        check(n == 512 && info.freq_hz == 0, "CSD1 -> implausible p3 leaves freq_hz at 0");

        // A CS16 block of the previous format is still read and does not fake a frequency.
        write_block(ring5.data(), l, 3, 4, 3000, 512, 0x77);
        n = w5.read(dst.data(), dst.size(), &info);
        check(n == 512 && dst[0] == 0x77, "RF30 -> the previous format is still read");
        check(info.format == fmiq::IqSampleFormat::Cs16 && info.sample_bytes == 4 && info.freq_hz == 0,
              "RF30 -> CS16 without a header frequency");
    }

    // Old dump: "CSD1" signature with an 8-bit payload. The forced value declares it without
    // looking at the content (on the real ring that byte-by-byte scan would be SIGBUS).
    {
        fmiq::IqRingLayout l8 = l;
        l8.force_format = fmiq::IqSampleFormat::Packed8;
        std::vector<std::uint8_t> ring8(l8.blocks * l8.block_bytes, 0);
        fmiq::IqWindow w8(l8);
        w8.attach_for_test(ring8.data(), ring8.size());
        write_packed_block(ring8.data(), l8, 0, l8.magic_packed, 1, 512, 21150u << 16);
        fmiq::IqBlockInfo info;
        check(w8.read(dst.data(), dst.size(), &info) == 512 &&
                  info.format == fmiq::IqSampleFormat::Packed8,
              "CSD1 + rx_format=packed8 -> read as 8 bits");

        // The CSD8 signature overrides any forced value.
        l8.force_format = fmiq::IqSampleFormat::Packed5;
        fmiq::IqWindow w8b(l8);
        w8b.attach_for_test(ring8.data(), ring8.size());
        write_packed_block(ring8.data(), l8, 0, l8.magic_packed8, 2, 512, 0);
        check(w8b.read(dst.data(), dst.size(), &info) == 512 &&
                  info.format == fmiq::IqSampleFormat::Packed8,
              "CSD8 -> the signature overrides force_format");
    }

    // Unpacking of the two 5-bit fields: I=+1, Q=-1 with codes 17/15.
    {
        const std::uint16_t w = ((17u & 0x1F) << 3) | ((15u & 0x1F) << 11);
        float i = 0, q = 0;
        fmiq::unpack5_f32(w, &i, &q);
        check(i == 1.0f / 16.0f && q == -1.0f / 16.0f, "unpack5: I and Q with offset 16 and scale 1/16");
        std::int16_t si = 0, sq = 0;
        fmiq::unpack5_s16(w, &si, &sq);
        check(si == 2048 && sq == -2048, "unpack5: conversion to CS16 (2048 per code)");
    }

    // 8-bit unpacking per component: I in the low byte, Q in the high byte, offset 128.
    // 0x40 = 64 -> -0.5; 0xC0 = 192 -> +0.5.
    {
        const std::uint16_t w = 0x40C0;                 // Q=0x40, I=0xC0
        float i = 0, q = 0;
        fmiq::unpack8_f32(w, &i, &q);
        check(i == 0.5f && q == -0.5f, "unpack8: I low byte, Q high byte, offset 128");
        std::int16_t si = 0, sq = 0;
        fmiq::unpack8_s16(w, &si, &sq);
        check(si == 16384 && sq == -16384, "unpack8: conversion to CS16 (256 per code)");
        // Byte extremes: 0 -> -1.0 and 255 -> +0.9921875.
        fmiq::unpack8_f32(0x00FF, &i, &q);
        check(i == 127.0f / 128.0f && q == -1.0f, "unpack8: extremes -1.0 and +0.9921875");
    }

    // Format detection by content: 5 bits leaves bits 0-2 and 8-10 at zero; 8 bits uses
    // them. Tested with synthetic words of both kinds.
    {
        std::uint8_t p5[128], p8[128];
        for (int k = 0; k < 64; ++k) {
            const std::uint16_t code = static_cast<std::uint16_t>(8 + (k % 17));
            const std::uint16_t w5 = static_cast<std::uint16_t>((code << 3) | ((code ^ 5) << 11));
            p5[2 * k] = static_cast<std::uint8_t>(w5 & 0xFF);
            p5[2 * k + 1] = static_cast<std::uint8_t>(w5 >> 8);
            const std::uint16_t w8 = static_cast<std::uint16_t>(k * 37 + 11);
            p8[2 * k] = static_cast<std::uint8_t>(w8 & 0xFF);
            p8[2 * k + 1] = static_cast<std::uint8_t>(w8 >> 8);
        }
        check(!fmiq::payload_is_packed8(p5, sizeof(p5)), "detection: 5 bits -> packed5");
        check(fmiq::payload_is_packed8(p8, sizeof(p8)), "detection: low bits used -> packed8");
        check(!fmiq::payload_is_packed8(p8, 8), "detection: too few words -> no decision");
    }

    std::printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
