// DeviceSource stub for when SoapySDR is not available in the build system.
// It keeps the same contract so the app compiles and reports a clear message instead of
// failing at link time.
#include "source.hpp"

namespace fmiq {

struct DeviceSource::Impl {};

DeviceSource::DeviceSource(std::string args, const ParamStore* params)
    : impl_(new Impl()), args_(std::move(args)), params_(params) {}

DeviceSource::~DeviceSource() = default;

bool DeviceSource::start(std::string& err) {
    err = "DeviceSource: built without SoapySDR; install libsoapysdr-dev or use --source synthetic";
    return false;
}

void DeviceSource::stop() {}

std::size_t DeviceSource::read(cplx*, std::size_t, int) { return 0; }

}  // namespace fmiq
