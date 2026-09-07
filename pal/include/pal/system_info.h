// pal/include/pal/system_info.h
// ISystemInfo：平台探针抽象接口（iot_monitor 的平台特定采集下沉目标）
// 铁律：本文件零平台依赖
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pal {

// 系统信息探针接口：由 pal/backends/* 提供具体实现
class ISystemInfo {
public:
    virtual ~ISystemInfo() = default;

    // 平台标识（如 "anyka-av100" / "linux-generic"）
    virtual std::string GetPlatformName() = 0;

    // 固件/系统版本号
    virtual std::string GetFirmwareVersion() = 0;

    // 设备唯一标识（SN/MAC 派生，实现自行决定来源）
    virtual std::string GetDeviceId() = 0;

    // 芯片温度（摄氏度）；不支持时返回 false
    virtual bool GetSocTemperature(float& celsius) = 0;
};

// 工厂：由当前编译的 backend 提供实现
std::unique_ptr<ISystemInfo> CreateSystemInfo();

} // namespace pal
