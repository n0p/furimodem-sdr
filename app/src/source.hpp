// IQ sample source. Several implementations coexist: synthetic, file and device (SoapySDR).
#pragma once

#include "fft.hpp"
#include "iq_format.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fmiq {

// Fixed display range (dBFS). Fft::magnitudesDb magnitudes are normalized to 0 dB for a
// full-scale carrier.
inline constexpr float kDbMin = -90.0f;
inline constexpr float kDbMax = -5.0f;

struct BandInfo {
    const char* name;
    double lo_hz;
    double hi_hz;
    double center_hz;
};

// Receive bands of interest for the modem. The centre is only the starting point of the demo.
inline constexpr BandInfo kBands[] = {
    {"B1", 2110.0e6, 2170.0e6, 2140.0e6},
    {"B3", 1805.0e6, 1880.0e6, 1842.5e6},
    {"B7", 2620.0e6, 2690.0e6, 2655.0e6},
    {"B20", 791.0e6, 821.0e6, 806.0e6},
    {"N78", 3300.0e6, 3800.0e6, 3500.0e6},
    {"ISM", 2400.0e6, 2483.5e6, 2440.0e6},
};
inline constexpr int kBandCount = int(sizeof(kBands) / sizeof(kBands[0]));

inline constexpr double kSampleRates[] = {1.92e6, 3.84e6, 7.68e6, 15.36e6};
inline constexpr int kSampleRateCount = int(sizeof(kSampleRates) / sizeof(kSampleRates[0]));

struct RxParams {
    double center_hz = kBands[0].center_hz;
    double sample_rate = kSampleRates[1];
    float gain_db = 12.0f;
    int band = 0;
};

// Reference gain: at this value the dB shift applied to the FFT row is 0, so the picture of a
// stream that was already working does not change. The GAIN control no longer touches the
// samples: pipeline.hpp adds (gain_db - kGainReferenceDb) dB to the finished row. Scaling and
// clipping samples used to create in-band spurs as soon as the control went up, and it could not
// lift the 8-bit path (26 dB below the 5-bit one) without crushing it against full scale.
inline constexpr float kGainReferenceDb = 12.0f;

// Parameters shared between UI, acquisition and FFT. Each field is atomic: there are no locks in
// the data path and one read per block is enough for the demo.
class ParamStore {
public:
    RxParams get() const {
        RxParams p;
        p.center_hz = center_.load(std::memory_order_relaxed);
        p.sample_rate = rate_.load(std::memory_order_relaxed);
        p.gain_db = gain_.load(std::memory_order_relaxed);
        p.band = band_.load(std::memory_order_relaxed);
        return p;
    }
    void set(const RxParams& p) {
        band_.store(p.band, std::memory_order_relaxed);
        gain_.store(p.gain_db, std::memory_order_relaxed);
        rate_.store(p.sample_rate, std::memory_order_relaxed);
        center_.store(p.center_hz, std::memory_order_relaxed);
    }
    void setCenter(double hz) { center_.store(hz, std::memory_order_relaxed); }
    void setSampleRate(double hz) { rate_.store(hz, std::memory_order_relaxed); }
    void setGain(float db) { gain_.store(db, std::memory_order_relaxed); }
    void setBand(int b) { band_.store(b, std::memory_order_relaxed); }

private:
    std::atomic<double> center_{kBands[0].center_hz};
    std::atomic<double> rate_{kSampleRates[1]};
    std::atomic<float> gain_{12.0f};
    std::atomic<int> band_{0};
};

// Acquisition interface. read() returns a block of up to max samples and may sleep up to
// timeout_ms waiting for the hardware; 0 means "no data yet".
//
// meta() is optional: sources that read a format with its own header (the modem ring or a block
// file) report the format and the burst frequency. The app uses those values for the axis; a
// source without them leaves the parameters as they are.
struct SourceMeta {
    bool valid = false;         // a header has been read
    bool follows_freq = false;  // the centre and rate come from each block
    double center_hz = 0;
    double sample_rate = 0;
    IqSampleFormat format = IqSampleFormat::Unknown;
    std::uint32_t seq = 0;
    std::uint32_t blocks = 0;
    std::uint32_t stale = 0;
};

class SampleSource {
public:
    virtual ~SampleSource() = default;
    virtual const char* name() const = 0;
    virtual bool start(std::string& err) = 0;
    virtual void stop() = 0;
    virtual std::size_t read(cplx* dst, std::size_t max, int timeout_ms) = 0;
    virtual bool meta(SourceMeta& out) const {
        (void)out;
        return false;
    }
    // Read-only diagnostic setting of the module (for example "rx_ring"). Sources without a
    // module say nothing.
    virtual std::string setting(const std::string& key) const {
        (void)key;
        return std::string();
    }
};

// Synthetic source: noise + fixed carriers of the band + a carrier sweeping the sample width +
// a cluster of tones that simulates a wide channel.
//
// It does not generate fs samples per second: it produces a block of `block` samples at 60
// blocks/s and maps the frequencies with the configured fs, so the FFT axis is correct without
// burning the phone CPU. DeviceSource replaces this producer without touching the pipeline.
class SyntheticSource final : public SampleSource {
public:
    SyntheticSource(const ParamStore& params, std::size_t block, bool paced = true)
        : params_(params), block_(block ? block : 2048), paced_(paced), rng_(0x12345678u) {
        t0_ = std::chrono::steady_clock::now();
        next_ = t0_;
        // Fixed carriers relative to the band centre (MHz): picture of an occupied channel.
        addTone(-2.4e6, 0.16f, 0.13);
        addTone(-0.6e6, 0.22f, 0.07);
        addTone(1.7e6, 0.12f, 0.19);
        // Wide cluster: 12 tones with different phases and amplitudes.
        for (int i = 0; i < 12; ++i) {
            const double off = (-5.0 + 10.0 * double(i) / 11.0) * 1e6;
            const float amp = 0.018f + 0.035f * float((i * 37) % 11) / 10.0f;
            addTone(off, amp, 0.03 + 0.02 * double(i % 4));
        }
        sweep_.off_hz = 0.0;
        sweep_.amp = 0.24f;
        sweep_.phase = 0.0;
        sweep_.am_rate = 0.0;
        sweep_.am_phase = 0.0;
    }

    const char* name() const override { return "synthetic"; }

    bool start(std::string& err) override {
        (void)err;
        t0_ = std::chrono::steady_clock::now();
        next_ = t0_;
        return true;
    }
    void stop() override {}

    std::size_t read(cplx* dst, std::size_t max, int /*timeout_ms*/) override {
        const std::size_t n = std::min(max, block_);
        const RxParams p = params_.get();
        const double fs = p.sample_rate > 1.0 ? p.sample_rate : 1.0;
        const double bc = kBands[std::max(0, std::min(p.band, kBandCount - 1))].center_hz;
        const double now_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();

        // Simplified receive filter: anything outside +/- 0.48*fs is not seen.
        struct Active {
            cplx ph;
            cplx rot;
            float amp;
        };
        Active act[kMaxTones];
        int nact = 0;

        const double edge = 0.48 * fs;
        for (int i = 0; i < ntones_; ++i) {
            Tone& t = tones_[i];
            const double f_abs = bc + t.off_hz;
            const double off = f_abs - p.center_hz;
            if (std::fabs(off) >= edge) continue;
            const double step = 2.0 * M_PI * off / fs;
            const float am = 0.72f + 0.28f * float(std::sin(2.0 * M_PI * t.am_rate * now_s + t.am_phase));
            act[nact++] = Active{std::polar(1.0f, float(t.phase)), std::polar(1.0f, float(step)),
                                 t.amp * am};
            t.phase = std::fmod(t.phase + step * double(n), 2.0 * M_PI);
        }

        // Carrier sweeping the visible width (period 9 s); it starts at the edge so it does not
        // leave a DC component in the first block.
        {
            const double f_abs = p.center_hz + 0.40 * fs * std::sin(2.0 * M_PI * (now_s / 9.0 + 0.25));
            const double step = 2.0 * M_PI * (f_abs - p.center_hz) / fs;
            act[nact++] = Active{std::polar(1.0f, float(sweep_.phase)), std::polar(1.0f, float(step)),
                                 sweep_.amp};
            sweep_.phase = std::fmod(sweep_.phase + step * double(n), 2.0 * M_PI);
        }

        for (std::size_t i = 0; i < n; ++i) {
            float re = 0.0f, im = 0.0f;
            for (int k = 0; k < nact; ++k) {
                re += act[k].amp * act[k].ph.real();
                im += act[k].amp * act[k].ph.imag();
                act[k].ph *= act[k].rot;
            }
            // Approximately Gaussian noise: sum of three uniform values.
            const float u = (float(nextU32() >> 8) * (1.0f / 16777216.0f) +
                             float(nextU32() >> 8) * (1.0f / 16777216.0f) +
                             float(nextU32() >> 8) * (1.0f / 16777216.0f) - 1.5f);
            re += u * 0.0012f;  // ~ -84 dBFS per bin with N=2048
            im += u * 0.0012f;
            // Soft ADC clipping. With no sample-domain gain this is a safety net only: the
            // GAIN control works on the dB row (pipeline.hpp) and never saturates the ADC.
            re = re > 1.0f ? 1.0f : (re < -1.0f ? -1.0f : re);
            im = im > 1.0f ? 1.0f : (im < -1.0f ? -1.0f : im);
            dst[i] = cplx(re, im);
        }

        // Demo pacing: 60 blocks/s, independent of fs (see the class comment). In the
        // verification modes (headless/gpu-test) it is generated as fast as possible.
        if (paced_) {
            next_ += std::chrono::microseconds(16667);
            const auto now = std::chrono::steady_clock::now();
            if (next_ < now) next_ = now;
            std::this_thread::sleep_until(next_);
        }
        return n;
    }

private:
    static constexpr int kMaxTones = 24;
    struct Tone {
        double off_hz = 0;
        float amp = 0;
        double phase = 0;
        double am_rate = 0;
        double am_phase = 0;
    };

    void addTone(double off_hz, float amp, double am_rate) {
        if (ntones_ >= kMaxTones) return;
        Tone& t = tones_[ntones_++];
        t.off_hz = off_hz;
        t.amp = amp;
        t.am_rate = am_rate;
        t.am_phase = double(nextU32() % 6283) / 1000.0;
        t.phase = double(nextU32() % 6283) / 1000.0;
    }

    uint32_t nextU32() {
        uint32_t x = rng_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng_ = x;
        return x;
    }

    const ParamStore& params_;
    std::size_t block_;
    bool paced_ = true;
    uint32_t rng_;
    Tone tones_[kMaxTones];
    int ntones_ = 0;
    Tone sweep_;
    std::chrono::steady_clock::time_point t0_, next_;
};

// Real source: talks to the project's SoapySDR module (driver=furimodem_iq). The implementation
// lives in source_device.cpp so the rest of the app does not depend on the SoapySDR headers.
class DeviceSource final : public SampleSource {
public:
    DeviceSource(std::string args, const ParamStore* params);
    ~DeviceSource() override;
    const char* name() const override { return "soapy"; }

    bool start(std::string& err) override;
    void stop() override;
    std::size_t read(cplx* dst, std::size_t max, int timeout_ms) override;
    bool meta(SourceMeta& out) const override;
    std::string setting(const std::string& key) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string args_;
    const ParamStore* params_;
};

// Source from a dump on disk. Two families, and the file decides which one:
//
//   - Blocks with a 0x80 B "CSD1" (5-bit I/Q) or "CSD8" (8-bit I/Q) header followed by their
//     payload: the same format the modem ring publishes, with the burst frequency in p3. This
//     is what tools/make-iqfile.py writes and what iqring_pull dumps on the phone.
//   - Interleaved CS16, exactly as the window delivers it in the previous format.
//
// It loops so the waterfall always has data.
class FileSource final : public SampleSource {
public:
    FileSource(std::string path, const ParamStore& params) : path_(std::move(path)), params_(&params) {}
    const char* name() const override { return "file"; }

    bool start(std::string& err) override {
        std::FILE* f = std::fopen(path_.c_str(), "rb");
        if (!f) {
            err = "FileSource: cannot open " + path_;
            return false;
        }
        std::vector<unsigned char> raw;
        unsigned char chunk[65536];
        std::size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) raw.insert(raw.end(), chunk, chunk + n);
        std::fclose(f);
        if (raw.size() < 4) {
            err = "FileSource: " + path_ + " has no samples";
            return false;
        }

        // The signature of the first header decides the family; no argument can force it.
        std::uint32_t sig = 0;
        std::memcpy(&sig, raw.data(), sizeof(sig));
        if ((sig == kMagicCsd1 || sig == kMagicCsd8) && parse_blocks(raw)) {
            raw_ = std::move(raw);
            packed_ = true;
            bi_ = bpos_ = 0;
            meta_ = SourceMeta{};
            meta_.valid = true;
            meta_.follows_freq = true;
            meta_.sample_rate = kPackedRateHz;
            meta_.format = blocks_[0].packed8 ? IqSampleFormat::Packed8 : IqSampleFormat::Packed5;
            update_meta();
            return true;
        }

        const std::size_t pairs = raw.size() / 4;
        samples_.resize(pairs);
        for (std::size_t i = 0; i < pairs; ++i) {
            const auto* p = reinterpret_cast<const std::int16_t*>(raw.data() + i * 4);
            samples_[i] = cplx(float(p[0]) / 32768.0f, float(p[1]) / 32768.0f);
        }
        pos_ = 0;
        return true;
    }

    void stop() override {}

    bool meta(SourceMeta& out) const override {
        if (!packed_) return false;
        out = meta_;
        return true;
    }

    std::size_t read(cplx* dst, std::size_t max, int) override {
        if (!dst || max == 0) return 0;
        if (!packed_) {
            if (samples_.empty()) return 0;
            for (std::size_t i = 0; i < max; ++i) {
                dst[i] = samples_[pos_];
                if (++pos_ >= samples_.size()) pos_ = 0;  // loop: the waterfall never runs dry
            }
            return max;
        }
        if (blocks_.empty()) return 0;

        std::size_t written = 0;
        while (written < max) {
            const Block& b = blocks_[bi_];
            const std::size_t avail = (b.bytes - bpos_) / 2;  // samples (2 B) remaining
            if (avail == 0) {
                advance();
                continue;
            }
            const std::size_t take = avail < (max - written) ? avail : (max - written);
            const std::uint8_t* src = raw_.data() + b.off + bpos_;
            for (std::size_t i = 0; i < take; ++i) {
                std::uint16_t w = 0;
                std::memcpy(&w, src + 2 * i, sizeof(w));
                float re = 0, im = 0;
                if (b.packed8) {
                    unpack8_f32(w, &re, &im);
                } else {
                    unpack5_f32(w, &re, &im);
                }
                dst[written + i] = cplx(re, im);
            }
            bpos_ += take * 2;  // both packed formats use 2 B per sample
            written += take;
            if (bpos_ >= b.bytes) advance();
        }
        update_meta();
        return written;
    }

private:
    struct Block {
        std::size_t off = 0;    // payload offset inside the file
        std::size_t bytes = 0;
        std::uint32_t seq = 0;
        std::uint64_t freq_hz = 0;
        bool csd8 = false;      // the block signature says 8 bits
        bool packed8 = false;   // effective format (signature or compatibility with old dumps)
    };

    // Walks the block sequence. Each one starts with its 0x80 B header; the payload size is
    // given by the +0x0C field. It stops at the first block that does not fit.
    bool parse_blocks(const std::vector<unsigned char>& raw) {
        std::size_t pos = 0;
        bool first_csd1 = false;
        while (pos + kBlockHeaderBytes <= raw.size()) {
            std::uint32_t words[5] = {0, 0, 0, 0, 0};
            std::memcpy(words, raw.data() + pos, sizeof(words));
            const std::uint32_t sig = words[0];
            if ((sig != kMagicCsd1 && sig != kMagicCsd8) || words[3] == 0) break;
            const std::size_t bytes = words[3];
            if (pos + kBlockHeaderBytes + bytes > raw.size()) break;
            std::uint32_t p3 = 0;
            std::memcpy(&p3, raw.data() + pos + 0x68, sizeof(p3));
            Block b;
            b.off = pos + kBlockHeaderBytes;
            b.bytes = bytes;
            b.seq = words[1];
            b.freq_hz = freq_hz_from_p3(p3);
            b.csd8 = (sig == kMagicCsd8);
            if (blocks_.empty()) first_csd1 = (sig == kMagicCsd1);
            blocks_.push_back(b);
            pos += kBlockHeaderBytes + bytes;
        }
        if (blocks_.empty()) return false;
        // Compatibility with dumps predating the "CSD8" signature: if the first block says
        // "CSD1" but uses the bits that the 5-bit packing leaves at zero, the whole file is read
        // as 8-bit. Only here, with the file already in RAM; the driver does not perform this
        // scan over the modem memory.
        legacy8_ = first_csd1 && payload_is_packed8(raw.data() + blocks_[0].off, blocks_[0].bytes);
        for (auto& b : blocks_) b.packed8 = b.csd8 || legacy8_;
        return true;
    }

    void advance() {
        ++bi_;
        if (bi_ >= blocks_.size()) bi_ = 0;
        bpos_ = 0;
        ++meta_.blocks;
    }

    void update_meta() {
        const Block& b = blocks_[bi_];
        meta_.seq = b.seq;
        meta_.format = b.packed8 ? IqSampleFormat::Packed8 : IqSampleFormat::Packed5;
        if (b.freq_hz) meta_.center_hz = double(b.freq_hz);
    }

    std::string path_;
    const ParamStore* params_;
    std::vector<cplx> samples_;      // CS16 of the previous format
    std::size_t pos_ = 0;
    std::vector<std::uint8_t> raw_;  // CSD1: whole file
    std::vector<Block> blocks_;
    std::size_t bi_ = 0, bpos_ = 0;
    bool packed_ = false;
    bool legacy8_ = false;  // file with a CSD1 signature and an 8-bit payload (old dump)
    SourceMeta meta_;
};

inline std::unique_ptr<SampleSource> makeSampleSource(const std::string& kind, const ParamStore& params,
                                                      std::size_t block, std::string& err,
                                                      const std::string& path = std::string(),
                                                      const std::string& device_args = std::string()) {
    if (kind.empty() || kind == "synthetic") return std::make_unique<SyntheticSource>(params, block);
    if (kind == "soapy" || kind == "device") return std::make_unique<DeviceSource>(device_args, &params);
    if (kind == "file") {
        if (path.empty()) {
            err = "the file source needs --file <path>";
            return nullptr;
        }
        return std::make_unique<FileSource>(path, params);
    }
    err = "unknown source: " + kind;
    return nullptr;
}

}  // namespace fmiq
