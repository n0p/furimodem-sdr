// Sample format published by the modem IQ ring.
//
// Three variants share the same 0x80 B block header and are told apart by the
// signature in the first u32:
//
//   "CSD1"  Packed 5-bit I/Q, one sample per 16-bit word:
//             I = bits 3..7 minus 16, Q = bits 11..15 minus 16.
//             Bits 0-2 and 8-10 are unused.
//
//   "CSD8"  8-bit I/Q per component, also one sample per word:
//             I = low byte minus 128, Q = high byte minus 128 (binary offset).
//             All 16 bits are data; range -128..127.
//
//   "RF30"  Interleaved CS16 (two int16 per sample), the previous format.
//
// The scanner window rate is 1.92 MS/s and the useful burst is 11,904 samples
// (6.2 ms) split into two blocks, for both packed formats.
//
// The burst frequency travels in the p3 field of the header: the high 16 bits in
// units of 100 kHz. p2 carries the next frequency of the sweep. With the 8-bit
// engine the frequency is fixed, but the field is still filled in and is the axis
// reference.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fmiq {

inline constexpr std::uint32_t kMagicCsd1 = 0x31445343u;  // "CSD1": 5-bit I/Q
inline constexpr std::uint32_t kMagicCsd8 = 0x38445343u;  // "CSD8": 8-bit I/Q
inline constexpr std::uint32_t kMagicRf30 = 0x30334652u;  // "RF30": CS16

inline constexpr std::size_t kBlockHeaderBytes = 0x80;
inline constexpr std::size_t kPacked5SampleBytes = 2;  // one 16-bit word
inline constexpr std::size_t kPacked8SampleBytes = 2;  // same: I in the low byte, Q in the high byte
inline constexpr std::size_t kCs16SampleBytes = 4;     // two int16

// Scanner window: 11,904 samples at 1.92 MS/s = 6.2 ms.
inline constexpr double kPackedRateHz = 1.92e6;
inline constexpr double kPacked5RateHz = kPackedRateHz;  // previous name, same rate
inline constexpr std::size_t kPackedWindowSamples = 11904;
inline constexpr std::size_t kPacked5WindowSamples = kPackedWindowSamples;
inline constexpr std::uint64_t kFreqUnitHz = 100000;  // p3 high 16 x 100 kHz

enum class IqSampleFormat : std::uint8_t { Unknown = 0, Cs16 = 1, Packed5 = 2, Packed8 = 3 };

inline IqSampleFormat format_from_magic(std::uint32_t magic) {
    if (magic == kMagicCsd1) return IqSampleFormat::Packed5;
    if (magic == kMagicCsd8) return IqSampleFormat::Packed8;
    if (magic == kMagicRf30) return IqSampleFormat::Cs16;
    return IqSampleFormat::Unknown;
}

// Both packed formats share the geometry (2 B/sample, 1.92 MS/s, burst metadata in
// the header); only the way the 16 bits of each sample are interpreted changes.
inline bool is_packed(IqSampleFormat f) {
    return f == IqSampleFormat::Packed5 || f == IqSampleFormat::Packed8;
}

inline std::size_t sample_bytes(IqSampleFormat f) {
    switch (f) {
        case IqSampleFormat::Packed5: return kPacked5SampleBytes;
        case IqSampleFormat::Packed8: return kPacked8SampleBytes;
        case IqSampleFormat::Cs16: return kCs16SampleBytes;
        default: return 0;
    }
}

inline double format_rate_hz(IqSampleFormat f) { return is_packed(f) ? kPackedRateHz : 0.0; }

inline const char* format_name(IqSampleFormat f) {
    switch (f) {
        case IqSampleFormat::Packed5: return "packed5";
        case IqSampleFormat::Packed8: return "packed8";
        case IqSampleFormat::Cs16: return "cs16";
        default: return "unknown";
    }
}

// Unpack one 16-bit word into a normalized complex float (-1..1).
// Full scale is 16 codes: the actual range is -16..15.
inline void unpack5_f32(std::uint16_t w, float* i, float* q) {
    *i = (static_cast<float>((w >> 3) & 0x1Fu) - 16.0f) * (1.0f / 16.0f);
    *q = (static_cast<float>((w >> 11) & 0x1Fu) - 16.0f) * (1.0f / 16.0f);
}

// The same sample in the CS16 container (16 codes -> 16 steps of 2048).
inline void unpack5_s16(std::uint16_t w, std::int16_t* i, std::int16_t* q) {
    *i = static_cast<std::int16_t>((static_cast<int>((w >> 3) & 0x1Fu) - 16) * 2048);
    *q = static_cast<std::int16_t>((static_cast<int>((w >> 11) & 0x1Fu) - 16) * 2048);
}

// 8 bits per component: I in the low byte, Q in the high byte, binary offset 128 (the
// byte is stored plus 128 and subtracted when unpacking). Range -128..127 -> -1.0..+0.9921875.
inline void unpack8_f32(std::uint16_t w, float* i, float* q) {
    *i = (static_cast<float>(w & 0xFFu) - 128.0f) * (1.0f / 128.0f);
    *q = (static_cast<float>((w >> 8) & 0xFFu) - 128.0f) * (1.0f / 128.0f);
}

inline void unpack8_s16(std::uint16_t w, std::int16_t* i, std::int16_t* q) {
    *i = static_cast<std::int16_t>((static_cast<int>(w & 0xFFu) - 128) * 256);
    *q = static_cast<std::int16_t>((static_cast<int>((w >> 8) & 0xFFu) - 128) * 256);
}

// Compatibility helper ONLY for files already loaded in RAM (old dumps that carry the
// "CSD1" signature but an 8-bit payload). It is never used on Device memory of the
// modem: a byte-by-byte access there can raise SIGBUS.
//
// In "CSD1" bits 0-2 and 8-10 of each word are zero; in 8-bit mode they are data and
// are 0/1 with probability ~1/2. Returns true if the sample is 8-bit. With fewer than
// 16 words it does not decide (leaves 5-bit).
inline bool payload_is_packed8(const void* payload, std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(payload);
    const std::size_t n = bytes / 2;
    if (n < 16) return false;
    const std::size_t step = n > 4096 ? n / 4096 : 1;   // spread sampling, cheap
    std::size_t seen = 0, zero = 0;
    for (std::size_t k = 0; k < n; k += step) {
        const std::uint16_t w = static_cast<std::uint16_t>(p[2 * k] | (p[2 * k + 1] << 8));
        if ((w & 0x0707u) == 0) ++zero;
        ++seen;
    }
    return seen != 0 && (static_cast<double>(zero) / static_cast<double>(seen)) < 0.90;
}

// Burst frequency in Hz from p3 (0 if the field is not plausible).
inline std::uint64_t freq_hz_from_p3(std::uint32_t p3) {
    const std::uint64_t units = p3 >> 16;              // units of 100 kHz
    if (units < 4000 || units > 60000) return 0;       // 400 MHz .. 6 GHz
    return units * kFreqUnitHz;
}

}  // namespace fmiq
