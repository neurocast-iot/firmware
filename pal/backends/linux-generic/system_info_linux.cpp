// linux-generic backend：宿主机系统探针（/proc、/sys 通用接口）
#include "pal/system_info.h"

#include <fstream>

namespace pal {

namespace {

class LinuxSystemInfo : public ISystemInfo {
public:
    std::string GetPlatformName() override { return "linux-generic"; }

    std::string GetFirmwareVersion() override {
        std::ifstream f("/proc/version");
        std::string line;
        if (f && std::getline(f, line)) {
            return line;
        }
        return "unknown";
    }

    std::string GetDeviceId() override {
        // 宿主机以 machine-id 作为设备标识
        std::ifstream f("/etc/machine-id");
        std::string id;
        if (f && std::getline(f, id)) {
            return id;
        }
        return "host-unknown";
    }

    bool GetSocTemperature(float& celsius) override {
        // thermal_zone0 常见于 x86/ARM 通用内核，读不到则不支持
        std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
        long milli = 0;
        if (f && (f >> milli)) {
            celsius = static_cast<float>(milli) / 1000.0f;
            return true;
        }
        return false;
    }
};

} // namespace

std::unique_ptr<ISystemInfo> CreateSystemInfo() {
    return std::make_unique<LinuxSystemInfo>();
}

} // namespace pal
