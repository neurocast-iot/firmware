/**
 * @file fw_handler.h
 * @brief 固件升级处理器
 *
 * 替换 /usr 下的系统文件（比如系统库、配置文件）。
 * 继承 UpgradeHandlerBase，复用共同流程（解压→备份→安装→校验→失败回滚）。
 * 只需要实现差异化部分：备份目录和日志前缀。
 *
 * 当前是文件级替换；未来如果硬件支持 A/B 分区，这里覆盖 executeUpgrade() 改成烧写分区就行，
 * 基类和 BackupManager 不动。
 */
#pragma once

#include "upgrade_handler_base.h"

#include <string>

namespace iot_agent {
namespace ota {

class FwHandler : public UpgradeHandlerBase {
public:
    /**
     * @param base_dir OTA 基础目录（比如 /mnt/emmc/ota）
     */
    explicit FwHandler(const std::string& base_dir)
        : m_base_dir(base_dir) {}

    // 复用基类的 executeUpgrade()，不需要覆盖
    // 未来如果要改成 A/B 分区烧写，在这里覆盖 executeUpgrade()

protected:
    std::string getBackupRoot() const override {
        return OtaPaths::fwBackup(m_base_dir);
    }

    const char* getLogPrefix() const override {
        return "[FwHandler]";
    }

private:
    std::string m_base_dir;
};

} // namespace ota
} // namespace iot_agent
