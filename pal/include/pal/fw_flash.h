// pal/include/pal/fw_flash.h
// IFwFlash：固件分区读写抽象接口（ota_agent 的 fw_handler 下沉目标）
// 铁律：本文件零平台依赖
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pal {

// 固件分区标识（AB 启动方案下由实现映射到具体 NAND 分区）
enum class FwSlot {
    kSlotA,
    kSlotB,
    kRecovery,
};

// 固件烧写接口：由 pal/backends/* 提供具体实现
class IFwFlash {
public:
    virtual ~IFwFlash() = default;

    // 查询当前运行槽位
    virtual FwSlot GetActiveSlot() = 0;

    // 将固件镜像文件烧写到指定槽位（阻塞，progress_cb 可为空）
    using ProgressCallback = void (*)(int percent, void* user_data);
    virtual bool WriteImage(FwSlot slot, const std::string& image_path,
                            ProgressCallback progress_cb, void* user_data) = 0;

    // 校验指定槽位固件完整性
    virtual bool VerifySlot(FwSlot slot) = 0;

    // 设置下次启动槽位（AB 切换）
    virtual bool SetBootSlot(FwSlot slot) = 0;
};

// 工厂：由当前编译的 backend 提供实现
std::unique_ptr<IFwFlash> CreateFwFlash();

} // namespace pal
