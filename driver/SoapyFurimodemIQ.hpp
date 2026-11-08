// SoapySDR driver for the device: a receive-only IQ source.
#pragma once
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace fmiq {

class SoapyFurimodemIQ : public SoapySDR::Device {
public:
    explicit SoapyFurimodemIQ(const SoapySDR::Kwargs& args);
    ~SoapyFurimodemIQ() override;

    std::string getDriverKey() const override;
    std::string getHardwareKey() const override;
    SoapySDR::Kwargs getHardwareInfo() const override;

    size_t getNumChannels(const int dir) const override;

    std::vector<std::string> getStreamFormats(const int direction, const size_t channel) const override;
    std::string getNativeStreamFormat(const int direction, const size_t channel, double& fullScale) const override;
    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction, const size_t channel) const override;

    SoapySDR::Stream* setupStream(const int direction, const std::string& format,
                                  const std::vector<size_t>& channels,
                                  const SoapySDR::Kwargs& args) override;
    void closeStream(SoapySDR::Stream* stream) override;
    int activateStream(SoapySDR::Stream* stream, const int flags, const long long timeNs, const size_t numElems) override;
    int deactivateStream(SoapySDR::Stream* stream, const int flags, const long long timeNs) override;
    int readStream(SoapySDR::Stream* stream, void* const* buffs, const size_t numElems,
                   int& flags, long long& timeNs, const long timeoutUs) override;
    std::vector<std::string> listAntennas(const int direction, const size_t channel) const override;
    void setAntenna(const int direction, const size_t channel, const std::string& name) override;
    std::string getAntenna(const int direction, const size_t channel) const override;

    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel) const override;
    void setFrequency(const int direction, const size_t channel, const double frequency,
                      const SoapySDR::Kwargs& args) override;
    double getFrequency(const int direction, const size_t channel) const override;

    std::vector<double> listSampleRates(const int direction, const size_t channel) const override;
    void setSampleRate(const int direction, const size_t channel, const double rate) override;
    double getSampleRate(const int direction, const size_t channel) const override;

    void setBandwidth(const int direction, const size_t channel, const double bw) override;
    double getBandwidth(const int direction, const size_t channel) const override;

    void setGain(const int direction, const size_t channel, const double gain) override;
    void setGain(const int direction, const size_t channel, const std::string& name, const double gain) override;
    double getGain(const int direction, const size_t channel, const std::string& name) const override;
    SoapySDR::Range getGainRange(const int direction, const size_t channel, const std::string& name) const override;
    void setGainMode(const int direction, const size_t channel, const bool automatic) override;
    bool getGainMode(const int direction, const size_t channel) const override;

    std::vector<std::string> listSensors() const override;
    SoapySDR::ArgInfo getSensorInfo(const std::string& key) const override;
    std::string readSensor(const std::string& key) const override;

    void writeSetting(const std::string& key, const std::string& value) override;
    std::string readSetting(const std::string& key) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    double freq_[2] = {2400e6, 2400e6};
    double rate_[2] = {20e6, 20e6};
    double gain_[2] = {20.0, 0.0};
    double bw_[2] = {20e6, 20e6};
    bool agc_[2] = {false, false};
    std::string antenna_ = "RX0";
};

} // namespace fmiq
