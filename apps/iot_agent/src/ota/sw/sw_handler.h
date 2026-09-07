/**
 * @file sw_handler.h
 * @brief 软件升级编排器
 *
 * 继承 UpgradeHandlerBase，复用共同流程（解压→备份→安装→校验→失败回滚）。
 * 只需要实现差异化部分：备份目录和日志前缀。
 */
#pragma once

#include "upgrade_handler_base.h"

#include <string>

namespace iot_agent {
namespace ota {

class SwHandler : public UpgradeHandlerBase {
public:
    /**
     * @param base_dir OTA 基础目录（比如 /mnt/emmc/ota）
     */
    explicit SwHandler(const std::string& base_dir)
        : m_base_dir(base_dir) {}

    // 复用基类的 executeUpgrade()，不需要覆盖

protected:
    std::string getBackupRoot() const override {
        return OtaPaths::swBackup(m_base_dir);
    }

    const char* getLogPrefix() const override {
        return "[SwHandler]";
    }

private:
    std::string m_base_dir;
};

} // namespace ota
} // namespace iot_agent
