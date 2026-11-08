// Minimal probe: checks that SoapySDR discovers the project module, opens the device, sets up the
// stream and answers a read. It needs neither the app nor a GPU.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>

#include <complex>
#include <cstdio>
#include <vector>

int main() {
    const auto devs = SoapySDR::Device::enumerate();
    std::printf("devices: %zu\n", devs.size());
    for (const auto& k : devs) {
        std::printf("  -");
        for (const auto& p : k) std::printf(" %s=%s", p.first.c_str(), p.second.c_str());
        std::printf("\n");
    }
    try {
        SoapySDR::Device* d = SoapySDR::Device::make("driver=furimodem_iq");
        std::printf("opened: %s\n", d->getHardwareKey().c_str());
        d->setSampleRate(SOAPY_SDR_RX, 0, 3.84e6);
        d->setFrequency(SOAPY_SDR_RX, 0, 4070.01e6);
        std::printf("freq=%.0f rate=%.0f\n", d->getFrequency(SOAPY_SDR_RX, 0),
                    d->getSampleRate(SOAPY_SDR_RX, 0));
        SoapySDR::Stream* s = d->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
        std::printf("stream: %s\n", s ? "ok" : "null");
        std::printf("activate: %d\n", d->activateStream(s));
        std::vector<std::complex<float>> buf(4096);
        void* b[1] = {buf.data()};
        int fl = 0;
        long long tn = 0;
        const int rc = d->readStream(s, b, buf.size(), fl, tn, 200000);
        std::printf("readStream -> %d\n", rc);
        d->deactivateStream(s);
        d->closeStream(s);
        SoapySDR::Device::unmake(d);
    } catch (const std::exception& e) {
        std::printf("exception: %s\n", e.what());
    }
    return 0;
}
