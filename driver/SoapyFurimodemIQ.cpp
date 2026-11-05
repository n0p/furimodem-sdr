#include "SoapyFurimodemIQ.hpp"
#include "ccb_transport.hpp"
#include "iq_window.hpp"
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace fmiq {
namespace {

// Unpacks a run of 16-bit words (two 5-bit fields) into complex float.
void unpack5_to_f32(const std::uint8_t* src, float* dst, std::size_t samples) {
    for (std::size_t i = 0; i < samples; ++i) {
        std::uint16_t w = 0;
        std::memcpy(&w, src + 2 * i, sizeof(w));
        unpack5_f32(w, &dst[2 * i], &dst[2 * i + 1]);
    }
}

// The same sample into the CS16 container requested by a client that does not use CF32.
void unpack5_to_s16(const std::uint8_t* src, std::int16_t* dst, std::size_t samples) {
    for (std::size_t i = 0; i < samples; ++i) {
        std::uint16_t w = 0;
        std::memcpy(&w, src + 2 * i, sizeof(w));
        unpack5_s16(w, &dst[2 * i], &dst[2 * i + 1]);
    }
}

// 8 bits per component (I in the low byte, Q in the high byte).
void unpack8_to_f32(const std::uint8_t* src, float* dst, std::size_t samples) {
    for (std::size_t i = 0; i < samples; ++i) {
        std::uint16_t w = 0;
        std::memcpy(&w, src + 2 * i, sizeof(w));
        unpack8_f32(w, &dst[2 * i], &dst[2 * i + 1]);
    }
}

void unpack8_to_s16(const std::uint8_t* src, std::int16_t* dst, std::size_t samples) {
    for (std::size_t i = 0; i < samples; ++i) {
        std::uint16_t w = 0;
        std::memcpy(&w, src + 2 * i, sizeof(w));
        unpack8_s16(w, &dst[2 * i], &dst[2 * i + 1]);
    }
}

bool flag_arg(const SoapySDR::Kwargs& args, const char* key, bool dflt) {
    auto it = args.find(key);
    if (it == args.end()) return dflt;
    return it->second == "1" || it->second == "true" || it->second == "yes";
}

std::string str_arg(const SoapySDR::Kwargs& args, const char* key, const char* dflt) {
    auto it = args.find(key);
    return (it == args.end() || it->second.empty()) ? std::string(dflt) : it->second;
}

// Why the memory window could not be mapped, as a token for readSetting("rx_ring"). It only
// classifies a failure that already happened: it changes nothing of what setupStream does. The
// node is probed read-only because IqWindow::open() reports the errno as text.
std::string ring_open_state(const std::string& node, const std::string& err) {
    if (::access(node.c_str(), F_OK) != 0) return "no-device";
    if (::access(node.c_str(), R_OK) != 0) return "no-permission";
    if (err.find("does not fit") != std::string::npos) return "no-layout";
    return "no-map";  // mmap/ioctl: the node exists and is readable but the window is not usable
}

}  // namespace

struct SoapyFurimodemIQ::Impl {
    bool rxActive = false, txActive = false;
    std::string rxFormat = SOAPY_SDR_CF32;
    // Receive transport (CCB META DL ring). It is only opened on
    // setupStream(RX): enumerating and probing the device does not touch any node.
    CcbOpenOptions opt;
    bool transportWanted = true;
    bool repeatLast = false;   // deliver the last block again if there are no new captures
    std::string iqNode = "/dev/ccci_raw_mdm";  // memory window (or file, for tests)
    long iqRingOff = -1;                       // <0 = default ring geometry
    IqSampleFormat forceFormat = IqSampleFormat::Unknown;  // force 5/8 bits (Unknown = auto)
    std::unique_ptr<CcbTransport> transport;
    // Direct reader of the IQ ring in shared memory (if the firmware writes it).
    std::unique_ptr<IqWindow> window;
    std::string transportError;
    // State of the memory window for readSetting("rx_ring"): "not-opened" until setupStream(RX),
    // then "ok" or the reason the mapping failed. Diagnostic only: it does not gate anything.
    std::string ringState = "not-opened";
    std::string ringDetail;
    // Format of the current block and metadata of the last burst. The ring may publish
    // CS16 (previous format) or packed 5/8-bit I/Q; the header signature decides.
    IqSampleFormat srcFormat = IqSampleFormat::Unknown;
    // Dumps with the "CSD1" signature and an 8-bit payload: the format is inferred ONCE
    // from the RAM copy of the block, never from the Device window.
    IqSampleFormat probedFormat = IqSampleFormat::Unknown;
    std::size_t srcSampleBytes = kCs16SampleBytes;
    IqBlockInfo lastBlock;
    bool hasBlock = false;
    std::uint64_t lastBurstFreqHz = 0;  // last non-zero frequency: the 2nd half of the block has p3=0
    bool followBlockFreq = true;  // getFrequency(RX) returns the burst frequency
    std::vector<uint8_t> scratch;  // read staging + conversion to the requested format
    size_t carryLen = 0;           // bytes already read that are not part of a delivery yet
};

SoapyFurimodemIQ::SoapyFurimodemIQ(const SoapySDR::Kwargs& args) : impl_(new Impl()) {
    lab_ = args.count("lab") && (args.at("lab") == "1" || args.at("lab") == "true");
    bool txEnable = args.count("tx_enable") && (args.at("tx_enable") == "1" || args.at("tx_enable") == "true");
    bool risky = args.count("allow_risky_bands") && (args.at("allow_risky_bands") == "1" ||
                                                     args.at("allow_risky_bands") == "true");
    TxPolicy p;
    p.build = lab_ ? Build::Lab : Build::Public;
    p.tx_enable = txEnable;
    if (lab_) p.allow_risky_bands = risky;
    guard_ = TxGuard(p);
    impl_->opt.node = str_arg(args, "ccb_node", "/dev/ccci_ccb_meta");
    impl_->opt.ctrl = str_arg(args, "ccb_ctrl", "/dev/ccci_ccb_ctrl");
    impl_->opt.simFile = str_arg(args, "ring_file", "");
    const std::string entry = str_arg(args, "ccb_entry", "-1");
    impl_->opt.entryHint = std::atoi(entry.c_str());
    impl_->transportWanted = flag_arg(args, "transport", true);
    impl_->repeatLast = flag_arg(args, "repeat_last", false);
    impl_->followBlockFreq = flag_arg(args, "rx_follow_block", true);
    impl_->iqNode = str_arg(args, "iq_node", "/dev/ccci_raw_mdm");
    impl_->iqRingOff = std::strtol(str_arg(args, "iq_ring_off", "-1").c_str(), nullptr, 0);
    // auto | packed5 | packed8. By default the block signature decides ("CSD1"/"CSD8");
    // the forced value only serves old dumps with a "CSD1" signature and an 8-bit payload.
    const std::string rf = str_arg(args, "rx_format", "auto");
    impl_->forceFormat = rf == "packed5"   ? IqSampleFormat::Packed5
                         : rf == "packed8" ? IqSampleFormat::Packed8
                                           : IqSampleFormat::Unknown;
    SoapySDR::logf(SOAPY_SDR_INFO, "%s", guard_.banner().c_str());
}

SoapyFurimodemIQ::~SoapyFurimodemIQ() = default;

std::string SoapyFurimodemIQ::getDriverKey() const { return "furimodem_iq"; }
std::string SoapyFurimodemIQ::getHardwareKey() const { return "furimodem"; }

SoapySDR::Kwargs SoapyFurimodemIQ::getHardwareInfo() const {
    SoapySDR::Kwargs info;
    info["driver"] = getDriverKey();
    info["hardware"] = getHardwareKey();
    info["build"] = lab_ ? "lab" : "public";
    info["tx"] = guard_.tx_channel_exists() ? "enabled" : "disabled";
    if (!impl_->transportWanted) info["rx_transport"] = "disabled";
    else if (impl_->transport) info["rx_transport"] = impl_->transport->sim() ? "sim" : "ccb-meta";
    else info["rx_transport"] = "none";
    if (impl_->hasBlock) {
        info["rx_format"] = format_name(impl_->srcFormat);
        info["rx_block_freq_hz"] = std::to_string(impl_->lastBlock.freq_hz);
        info["rx_block_seq"] = std::to_string(impl_->lastBlock.seq);
    }
    return info;
}

size_t SoapyFurimodemIQ::getNumChannels(const int dir) const {
    if (dir == SOAPY_SDR_RX) return 1;
    return guard_.tx_channel_exists() ? 1 : 0;
}

std::vector<std::string> SoapyFurimodemIQ::getStreamFormats(const int, const size_t) const {
    return {SOAPY_SDR_CF32};
}

std::string SoapyFurimodemIQ::getNativeStreamFormat(const int, const size_t, double& fullScale) const {
    fullScale = 32767.0;
    return SOAPY_SDR_CS16;
}

SoapySDR::ArgInfoList SoapyFurimodemIQ::getStreamArgsInfo(const int, const size_t) const {
    return SoapySDR::ArgInfoList();
}

SoapySDR::Stream* SoapyFurimodemIQ::setupStream(const int direction, const std::string& format,
                                                const std::vector<size_t>& channels,
                                                const SoapySDR::Kwargs& args) {
    if (direction == SOAPY_SDR_TX && !guard_.tx_channel_exists())
        throw std::runtime_error("TX channel does not exist (tx_enable=0)");
    if (format != SOAPY_SDR_CF32 && format != SOAPY_SDR_CS16)
        throw std::runtime_error("unsupported stream format");
    (void)channels; (void)args;
    if (direction == SOAPY_SDR_RX) {
        impl_->rxFormat = format;
        impl_->carryLen = 0;
        impl_->srcFormat = IqSampleFormat::Unknown;
        impl_->probedFormat = IqSampleFormat::Unknown;
        impl_->srcSampleBytes = kCs16SampleBytes;
        impl_->hasBlock = false;
        impl_->lastBlock = IqBlockInfo{};
        // Direct IQ ring reader: if the firmware leaves blocks, they are used before the CCB.
        if (!impl_->window) {
            std::string werr;
            IqRingLayout lay;
            if (impl_->iqRingOff >= 0) lay.window_off = static_cast<std::size_t>(impl_->iqRingOff);
            lay.force_format = impl_->forceFormat;
            auto w = std::unique_ptr<IqWindow>(new IqWindow(lay));
            w->set_repeat_last(impl_->repeatLast);
            if (w->open(impl_->iqNode, &werr)) {
                SoapySDR::logf(SOAPY_SDR_INFO, "RX: IQ ring ready (%s)", w->description().c_str());
                impl_->window = std::move(w);
                impl_->ringState = "ok";
                impl_->ringDetail.clear();
            } else {
                // Same behaviour as before (the CCB transport is still tried); only the reason
                // is kept so the app can tell "not mapped" from "mapped without sweep".
                impl_->ringState = ring_open_state(impl_->iqNode, werr);
                impl_->ringDetail = werr;
                SoapySDR::logf(SOAPY_SDR_DEBUG, "RX: no IQ ring (%s)", werr.c_str());
            }
        }
        if (impl_->transportWanted && !impl_->transport) {
            std::string err;
            impl_->transport = CcbTransport::open(impl_->opt, &err);
            if (impl_->transport) {
                SoapySDR::logf(SOAPY_SDR_INFO, "RX: CCB transport ready (%s)",
                               impl_->transport->description().c_str());
            } else {
                impl_->transportError = err;
                SoapySDR::logf(SOAPY_SDR_WARNING,
                               "RX: no transport (%s); readStream will return TIMEOUT",
                               err.c_str());
            }
        }
    }
    return (SoapySDR::Stream*)(uintptr_t)(direction == SOAPY_SDR_TX ? 2 : 1);
}

void SoapyFurimodemIQ::closeStream(SoapySDR::Stream*) {
    impl_->carryLen = 0;
    impl_->transport.reset();  // the node is reopened on the next setupStream(RX)
}

int SoapyFurimodemIQ::activateStream(SoapySDR::Stream* stream, const int flags, const long long, const size_t) {
    const bool isTx = (uintptr_t)stream == 2;
    if (isTx) {
        if (!guard_.tx_channel_exists()) return SOAPY_SDR_NOT_SUPPORTED;
        armed_[SOAPY_SDR_TX] = true;
        guard_.arm();
    } else {
        impl_->rxActive = true;
    }
    (void)flags;
    SoapySDR::logf(SOAPY_SDR_INFO, "activateStream(dir=%s)", isTx ? "TX" : "RX");
    return 0;
}

int SoapyFurimodemIQ::deactivateStream(SoapySDR::Stream* stream, const int, const long long) {
    const bool isTx = (uintptr_t)stream == 2;
    if (isTx) { armed_[SOAPY_SDR_TX] = false; guard_.disarm(); }
    else { impl_->rxActive = false; impl_->carryLen = 0; }
    return 0;
}

int SoapyFurimodemIQ::readStream(SoapySDR::Stream* stream, void* const* buffs, const size_t numElems,
                                 int& flags, long long& timeNs, const long timeoutUs) {
    flags = 0;
    timeNs = 0;
    if ((uintptr_t)stream != 1) return SOAPY_SDR_NOT_SUPPORTED;
    Impl& im = *impl_;
    // With no node (or transport=0) the driver stays alive: timeout, never fake data.
    // It waits a little (10 ms cap) so a client loop does not spin.
    if (!im.transport && !im.window) {
        if (timeoutUs > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(std::min<long>(timeoutUs, 10000)));
        return SOAPY_SDR_TIMEOUT;
    }
    if (!im.rxActive) return SOAPY_SDR_NOT_SUPPORTED;
    if (!buffs || !buffs[0] || numElems == 0) return 0;

    const bool cf32 = (im.rxFormat == SOAPY_SDR_CF32);
    constexpr size_t kMaxSrcSample = 4;              // the larger of the two formats
    constexpr size_t kMaxScratch = size_t(1) << 20;  // 1 MiB requested per read
    // Large block: it is split (and the multiplication never overflows). Bytes are requested
    // as if the sample were CS16: with a packed format (2 B/sample) twice as many samples as
    // requested arrive and the surplus stays for the next call.
    const size_t want = numElems > (kMaxScratch / kMaxSrcSample) ? kMaxScratch : numElems * kMaxSrcSample;
    const size_t need = want + 65536;  // room for the whole block on top of what was requested
    if (im.scratch.size() < need) im.scratch.resize(need);  // grows only, never per call

    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const size_t have = im.carryLen;
        int n = 0;
        // If the surplus already covers the request the ring is not touched: drain first.
        const size_t covered = numElems > (kMaxScratch / kMaxSrcSample)
                                   ? kMaxScratch
                                   : numElems * (im.srcSampleBytes ? im.srcSampleBytes : kCs16SampleBytes);
        if (have < covered) {
            if (im.window) {
                IqBlockInfo info;
                n = static_cast<int>(im.window->read(im.scratch.data() + have, want, &info));
                if (n > 0) {
                    // The signature rules. Only if the block says "CSD1" and nobody forced the
                    // format is the copy already made in RAM inspected (never the Device
                    // window): it is the compatibility net for the image that still publishes
                    // 8 bits under "CSD1".
                    if (info.format == IqSampleFormat::Packed5 &&
                        im.forceFormat == IqSampleFormat::Unknown) {
                        if (im.probedFormat == IqSampleFormat::Unknown && n >= 512) {
                            im.probedFormat = payload_is_packed8(im.scratch.data() + have,
                                                                 static_cast<std::size_t>(n))
                                                  ? IqSampleFormat::Packed8
                                                  : IqSampleFormat::Packed5;
                        }
                        if (im.probedFormat == IqSampleFormat::Packed8)
                            info.format = IqSampleFormat::Packed8;
                    }
                    // The capture spans two blocks and only the first one carries the page
                    // frequency (p3). The second half keeps the one of the burst in progress so
                    // getFrequency does not jump to the requested frequency between halves.
                    if (info.freq_hz != 0) {
                        im.lastBurstFreqHz = info.freq_hz;
                    } else if (is_packed(info.format)) {
                        info.freq_hz = im.lastBurstFreqHz;
                        info.next_freq_hz = info.next_freq_hz ? info.next_freq_hz : im.lastBurstFreqHz;
                    }
                    im.lastBlock = info;
                    im.hasBlock = true;
                    im.srcFormat = info.format;
                    im.srcSampleBytes = info.sample_bytes ? info.sample_bytes : kCs16SampleBytes;
                }
            }
            if (n <= 0 && im.transport) {
                n = im.transport->read(im.scratch.data() + have, want);
                if (n > 0) {  // the CCB transport delivers CS16
                    im.srcFormat = IqSampleFormat::Cs16;
                    im.srcSampleBytes = kCs16SampleBytes;
                }
            }
        }
        if (n > 0 || have > 0) {
            const size_t total = have + static_cast<size_t>(n > 0 ? n : 0);
            const size_t sb = im.srcSampleBytes ? im.srcSampleBytes : kCs16SampleBytes;
            size_t samples = total / sb;
            if (samples > numElems) samples = numElems;
            if (samples > 0) {
                const size_t used = samples * sb;
                const std::uint8_t* src = im.scratch.data();
                if (im.srcFormat == IqSampleFormat::Packed5) {
                    if (cf32) {
                        unpack5_to_f32(src, static_cast<float*>(buffs[0]), samples);
                    } else {
                        unpack5_to_s16(src, static_cast<std::int16_t*>(buffs[0]), samples);
                    }
                } else if (im.srcFormat == IqSampleFormat::Packed8) {
                    if (cf32) {
                        unpack8_to_f32(src, static_cast<float*>(buffs[0]), samples);
                    } else {
                        unpack8_to_s16(src, static_cast<std::int16_t*>(buffs[0]), samples);
                    }
                } else if (cf32) {
                    const std::int16_t* s16 = reinterpret_cast<const std::int16_t*>(src);
                    float* dst = static_cast<float*>(buffs[0]);
                    for (size_t i = 0; i < samples * 2; ++i)
                        dst[i] = static_cast<float>(s16[i]) * (1.0f / 32768.0f);
                } else {
                    std::memcpy(buffs[0], src, used);
                }
                const size_t rest = total - used;  // whole samples left over
                if (rest) std::memmove(im.scratch.data(), im.scratch.data() + used, rest);
                im.carryLen = rest;
                if (rest == 0) flags |= SOAPY_SDR_END_BURST;  // end of the block burst
                return static_cast<int>(samples);
            }
            im.carryLen = total;  // does not make up a complete sample
        }
        if (timeoutUs <= 0) return SOAPY_SDR_TIMEOUT;
        const long long elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= timeoutUs) return SOAPY_SDR_TIMEOUT;
        const long rest = timeoutUs - static_cast<long>(elapsed);
        std::this_thread::sleep_for(std::chrono::microseconds(std::min<long>(100, rest)));
    }
}

int SoapyFurimodemIQ::writeStream(SoapySDR::Stream* stream, const void* const*, const size_t numElems,
                                  int&, const long long, const long) {
    if ((uintptr_t)stream != 2) return SOAPY_SDR_NOT_SUPPORTED;
    const Verdict v = guard_.check_write(numElems);
    if (v != Verdict::Ok) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "writeStream rejected: %s", verdict_str(v));
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    // No active transport: the block is accepted and dropped (there is no RF yet).
    return (int)guard_.clamp_write(numElems);
}

std::vector<std::string> SoapyFurimodemIQ::listAntennas(const int dir, const size_t) const {
    if (dir == SOAPY_SDR_TX) return guard_.tx_channel_exists() ? std::vector<std::string>{"TX0"} : std::vector<std::string>{};
    return {"RX0"};
}
void SoapyFurimodemIQ::setAntenna(const int dir, const size_t, const std::string& name) { antenna_[dir] = name; }
std::string SoapyFurimodemIQ::getAntenna(const int dir, const size_t) const { return antenna_[dir]; }

SoapySDR::RangeList SoapyFurimodemIQ::getFrequencyRange(const int dir, const size_t) const {
    if (dir == SOAPY_SDR_TX && !guard_.tx_channel_exists()) return {};
    return {SoapySDR::Range(400e6, 6000e6)};
}

void SoapyFurimodemIQ::setFrequency(const int dir, const size_t, const double frequency, const SoapySDR::Kwargs&) {
    if (dir == SOAPY_SDR_TX) {
        const Verdict v = guard_.check_frequency(frequency);
        if (v != Verdict::Ok) throw std::runtime_error(std::string("setFrequency(TX): ") + verdict_str(v));
    }
    freq_[dir] = frequency;
}

// On receive the sweep is driven by the firmware: each block carries its frequency in the
// header. getFrequency(RX) returns the one of the last delivered burst (rx_follow_block=1 by
// default), so the client axis follows the data and not what was requested in setFrequency.
double SoapyFurimodemIQ::getFrequency(const int dir, const size_t) const {
    if (dir == SOAPY_SDR_RX && impl_->followBlockFreq && impl_->hasBlock && impl_->lastBlock.freq_hz != 0)
        return static_cast<double>(impl_->lastBlock.freq_hz);
    return freq_[dir];
}

std::vector<double> SoapyFurimodemIQ::listSampleRates(const int, const size_t) const {
    return {kPackedRateHz, 0.5e6, 1e6, 5e6, 10e6, 20e6, 40e6};
}
void SoapyFurimodemIQ::setSampleRate(const int dir, const size_t, const double rate) { rate_[dir] = rate; }
double SoapyFurimodemIQ::getSampleRate(const int dir, const size_t) const {
    // The scanner window has a fixed rate; the real one is reported, not the requested one.
    if (dir == SOAPY_SDR_RX) {
        const double packed = format_rate_hz(impl_->srcFormat);
        if (packed > 0) return packed;
    }
    return rate_[dir];
}

void SoapyFurimodemIQ::setBandwidth(const int dir, const size_t, const double bw) { bw_[dir] = bw; }
double SoapyFurimodemIQ::getBandwidth(const int dir, const size_t) const { return bw_[dir]; }

void SoapyFurimodemIQ::setGain(const int dir, const size_t, const double gain) {
    if (dir == SOAPY_SDR_TX) {
        const Verdict v = guard_.check_gain(gain);
        if (v != Verdict::Ok) throw std::runtime_error(std::string("setGain(TX): ") + verdict_str(v));
    }
    gain_[dir] = gain;
}
void SoapyFurimodemIQ::setGain(const int dir, const size_t ch, const std::string&, const double gain) {
    setGain(dir, ch, gain);
}
double SoapyFurimodemIQ::getGain(const int dir, const size_t, const std::string&) const { return gain_[dir]; }
SoapySDR::Range SoapyFurimodemIQ::getGainRange(const int dir, const size_t, const std::string&) const {
    if (dir == SOAPY_SDR_TX) return SoapySDR::Range(0.0, lab_ ? 40.0 : 10.0);
    return SoapySDR::Range(0.0, 40.0);
}
void SoapyFurimodemIQ::setGainMode(const int dir, const size_t, const bool automatic) { agc_[dir] = automatic; }
bool SoapyFurimodemIQ::getGainMode(const int dir, const size_t) const { return agc_[dir]; }

std::vector<std::string> SoapyFurimodemIQ::listSensors() const {
    return {"RSSI", "TEMP",       "LINK_ERR", "OVF",       "RX_PENDING",
            "RX_DROPPED", "RX_BAD", "RX_FREQ",  "RX_RATE",   "RX_SEQ",
            "RX_FORMAT",  "RX_STALE"};
}

SoapySDR::ArgInfo SoapyFurimodemIQ::getSensorInfo(const std::string& key) const {
    SoapySDR::ArgInfo a;
    a.key = key;
    a.type = SoapySDR::ArgInfo::FLOAT;
    return a;
}

std::string SoapyFurimodemIQ::readSensor(const std::string& key) const {
    if (key == "RSSI") return "-120.0 dBm";
    if (key == "TEMP") return "0.0 C";
    if (key == "LINK_ERR" || key == "OVF") return "0";
    if (key == "RX_PENDING")
        return std::to_string(impl_->transport ? impl_->transport->pendingPages() : 0);
    if (key == "RX_DROPPED")
        return std::to_string(impl_->transport ? impl_->transport->droppedPages() : 0);
    if (key == "RX_BAD") return std::to_string(impl_->transport ? impl_->transport->badPages() : 0);
    // Ring metadata: the frequency and format of the last delivered block.
    if (key == "RX_FREQ") return std::to_string(impl_->lastBlock.freq_hz);
    if (key == "RX_RATE")
        return std::to_string(format_rate_hz(impl_->srcFormat));
    if (key == "RX_SEQ") return std::to_string(impl_->lastBlock.seq);
    if (key == "RX_FORMAT") return format_name(impl_->srcFormat);
    if (key == "RX_STALE") return std::to_string(impl_->lastBlock.stale);
    return "";
}

void SoapyFurimodemIQ::writeSetting(const std::string& key, const std::string& value) {
    if (key == "rx_follow_block") {
        impl_->followBlockFreq = !(value == "0" || value == "false" || value == "no");
        SoapySDR::logf(SOAPY_SDR_INFO, "rx_follow_block=%d", impl_->followBlockFreq ? 1 : 0);
        return;
    }
    SoapySDR::logf(SOAPY_SDR_DEBUG, "writeSetting(%s=%s)", key.c_str(), value.c_str());
}

std::string SoapyFurimodemIQ::readSetting(const std::string& key) const {
    if (key == "rx_format") return format_name(impl_->srcFormat);
    if (key == "rx_follow_block") return impl_->followBlockFreq ? "1" : "0";
    // State of the direct IQ ring. Tokens: ok | not-opened | no-device | no-permission | no-map |
    // no-layout | no-blocks | no-format, optionally followed by ": " and a human detail.
    // Read-only: it reports, it never changes what the stream does.
    if (key == "rx_ring") {
        std::string state = impl_->ringState;
        std::string detail = impl_->ringDetail;
        if (impl_->window) {
            const IqWindow::RingProbe pr = impl_->window->probe();
            if (pr.blocks_valid > 0) {
                state = "ok";
                detail.clear();
            } else if (pr.blocks_unknown > 0) {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "%u block(s) with signature 0x%08x",
                              pr.blocks_unknown, pr.unknown_signature);
                state = "no-format";
                detail = buf;
            } else {
                state = "no-blocks";
                detail.clear();
            }
        }
        return detail.empty() ? state : state + ": " + detail;
    }
    if (key == "rx_block") {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "seq=%u blk=%u bytes=%u freq_hz=%llu next_hz=%llu push=%u slot=%u stale=%u "
                      "format=%s rate=%.0f",
                      impl_->lastBlock.seq, impl_->lastBlock.blk, impl_->lastBlock.bytes,
                      static_cast<unsigned long long>(impl_->lastBlock.freq_hz),
                      static_cast<unsigned long long>(impl_->lastBlock.next_freq_hz),
                      impl_->lastBlock.push, impl_->lastBlock.slot, impl_->lastBlock.stale,
                      format_name(impl_->srcFormat),
                      format_rate_hz(impl_->srcFormat));
        return std::string(buf);
    }
    return "";
}

} // namespace fmiq

static SoapySDR::KwargsList findFurimodemIQ(const SoapySDR::Kwargs& args) {
    SoapySDR::KwargsList results;
    // A single device; it can be filtered by serial if one is passed.
    if (args.count("serial")) return results;
    SoapySDR::Kwargs k;
    k["driver"] = "furimodem_iq";
    k["label"] = "Furimodem IQ";
    results.push_back(k);
    return results;
}

static SoapySDR::Device* makeFurimodemIQ(const SoapySDR::Kwargs& args) {
    return new fmiq::SoapyFurimodemIQ(args);
}

static SoapySDR::Registry registerFurimodemIQ("furimodem_iq", &findFurimodemIQ, &makeFurimodemIQ,
                                              SOAPY_SDR_ABI_VERSION);
