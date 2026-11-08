// Real IQ source: opens the project's SoapySDR module and reads CF32 directly into the
// destination. This is the only file in the app that depends on SoapySDR, and it is only
// compiled when the library is available (see Makefile/CMakeLists).
#include "source.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Modules.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace fmiq {

struct DeviceSource::Impl {
    SoapySDR::Device* dev = nullptr;
    SoapySDR::Stream* stream = nullptr;
    bool streaming = false;

    // Last values applied to the device: rewritten only when the user changes them.
    double applied_hz = -1.0;
    double applied_rate = -1.0;
    double applied_gain = -1e9;

    // With a packed format the sweep is driven by the firmware: each block carries its
    // frequency in the header and getFrequency(RX) returns it. The request is still pushed so
    // that a module which does accept host tuning is used, and the read-back is the verdict:
    // if the header keeps returning another frequency, the app says on screen that the
    // activated profile controls the receiver.
    bool follows_block = false;
    int probe_countdown = 0;
    SourceMeta meta;

    void apply(const RxParams& p) {
        if (!dev) return;
        // Both are pushed even with a block-driven format. setFrequency used to be skipped here
        // on purpose, which left the controls without any effect at all; now the module can take
        // it and, when it does not, the app sees the block frequency and reports it.
        //
        // Values that came from the block header itself are not pushed back: the UI writes them
        // into the store every block, and re-commanding the frequency the firmware is already
        // delivering would be noise (and, with a sweep, one setFrequency per block).
        const bool from_block_rate =
            follows_block && meta.sample_rate > 0 && std::fabs(p.sample_rate - meta.sample_rate) <= 1.0;
        const bool from_block_center =
            follows_block && meta.center_hz > 0 && std::fabs(p.center_hz - meta.center_hz) <= 0.5;
        if (p.sample_rate != applied_rate) {
            if (!from_block_rate) dev->setSampleRate(SOAPY_SDR_RX, 0, p.sample_rate);
            applied_rate = p.sample_rate;
        }
        if (p.center_hz != applied_hz) {
            if (!from_block_center) dev->setFrequency(SOAPY_SDR_RX, 0, p.center_hz);
            applied_hz = p.center_hz;
        }
        if (p.gain_db != applied_gain) {
            dev->setGain(SOAPY_SDR_RX, 0, double(p.gain_db));
            applied_gain = p.gain_db;
        }
    }
};

DeviceSource::DeviceSource(std::string args, const ParamStore* params)
    : impl_(new Impl()), args_(std::move(args)), params_(params) {}

DeviceSource::~DeviceSource() { stop(); }

bool DeviceSource::start(std::string& err) {
    try {
        // Our module is always requested; extra arguments are appended without overriding the key.
        std::string args = "driver=furimodem_iq";
        if (!args_.empty()) {
            if (args_.find("driver=") != std::string::npos) {
                args = args_;
            } else {
                args += "," + args_;
            }
        }
        impl_->dev = SoapySDR::Device::make(args);
        if (!impl_->dev) {
            err = "DeviceSource: cannot open the device (" + args + ")";
            return false;
        }
        if (params_) impl_->apply(params_->get());

        impl_->stream = impl_->dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
        if (!impl_->stream) {
            err = "DeviceSource: setupStream(CF32) returned no stream";
            SoapySDR::Device::unmake(impl_->dev);
            impl_->dev = nullptr;
            return false;
        }
        const int rc = impl_->dev->activateStream(impl_->stream);
        if (rc != 0) {
            err = std::string("DeviceSource: activateStream failed: ") + SoapySDR::errToStr(rc);
            impl_->dev->closeStream(impl_->stream);
            impl_->stream = nullptr;
            SoapySDR::Device::unmake(impl_->dev);
            impl_->dev = nullptr;
            return false;
        }
        impl_->streaming = true;
        return true;
    } catch (const std::exception& e) {
        err = std::string("DeviceSource: ") + e.what();
        return false;
    }
}

void DeviceSource::stop() {
    if (!impl_ || !impl_->dev) return;
    try {
        if (impl_->streaming && impl_->stream) {
            impl_->dev->deactivateStream(impl_->stream);
            impl_->streaming = false;
        }
        if (impl_->stream) {
            impl_->dev->closeStream(impl_->stream);
            impl_->stream = nullptr;
        }
        SoapySDR::Device::unmake(impl_->dev);
    } catch (...) {
        // stop() does not propagate: it is called from the destructor and from the orderly close.
    }
    impl_->dev = nullptr;
}

std::size_t DeviceSource::read(cplx* dst, std::size_t max, int timeout_ms) {
    if (!impl_ || !impl_->dev || !impl_->streaming || !dst || max == 0) return 0;
    if (params_) impl_->apply(params_->get());

    void* buffs[1] = {reinterpret_cast<void*>(dst)};
    int flags = 0;
    long long time_ns = 0;
    const long timeout_us = timeout_ms > 0 ? long(timeout_ms) * 1000L : 0L;

    const int rc = impl_->dev->readStream(impl_->stream, buffs, max, flags, time_ns, timeout_us);
    if (rc > 0) {
        // The module publishes the format it is delivering. With a packed format the frequency
        // and rate come from each block header; the request was still pushed by apply(), so the
        // app can compare it with the read-back and report a profile that ignores it.
        if (!impl_->follows_block && --impl_->probe_countdown <= 0) {
            const std::string fmt = impl_->dev->readSetting("rx_format");
            if (fmt == "packed5" || fmt == "packed8") {
                impl_->follows_block = true;
                impl_->meta.valid = true;
                impl_->meta.follows_freq = true;
                impl_->meta.format = fmt == "packed8" ? IqSampleFormat::Packed8
                                                      : IqSampleFormat::Packed5;
            }
            impl_->probe_countdown = 16;
        }
        if (impl_->follows_block) {
            impl_->meta.center_hz = impl_->dev->getFrequency(SOAPY_SDR_RX, 0);
            impl_->meta.sample_rate = impl_->dev->getSampleRate(SOAPY_SDR_RX, 0);
            impl_->meta.blocks += 1;
            const std::string s = impl_->dev->readSetting("rx_block");
            const auto key = s.find("seq=");
            if (key != std::string::npos) impl_->meta.seq = std::uint32_t(std::strtoul(s.c_str() + key + 4, nullptr, 10));
        }
        return std::size_t(rc);
    }
    // TIMEOUT and OVERFLOW are not failures: the app loop retries.
    if (rc == SOAPY_SDR_TIMEOUT || rc == SOAPY_SDR_OVERFLOW) return 0;
    return 0;
}

bool DeviceSource::meta(SourceMeta& out) const {
    if (!impl_ || !impl_->meta.valid) return false;
    out = impl_->meta;
    return true;
}

// Read-only. readSetting("rx_ring") reports whether the memory window is mapped and what is being
// published on it; it never changes what the stream does.
std::string DeviceSource::setting(const std::string& key) const {
    if (!impl_ || !impl_->dev) return std::string();
    return impl_->dev->readSetting(key);
}

}  // namespace fmiq
