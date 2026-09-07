/**
 * @file local_upgrade_trigger.h
 * @brief 本地升级触发器 —— inbox 目录扫描
 *
 * 使用场景：开发/运维人员用 SCP 把升级包传到设备的 inbox 目录，
 * 然后 kill -SIGUSR1 <pid>（软件升级）或 kill -SIGUSR2 <pid>（固件升级），
 * SignalManager 收到信号后设置标志位，主循环 poll() 扫描 inbox，
 * 把最新的升级包交给 OtaManager。
 *
 * 为什么信号处理在 SignalManager：
 *   统一信号管理，避免分散注册导致冲突。
 *
 * 为什么从 main.cpp 搬出来：
 *   inbox 扫描是 OTA 业务的细节，main 只负责"创建 + 周期 poll"。
 */
#pragma once

#include <string>

namespace iot_agent {
namespace ota {
class OtaManager;
}

class LocalUpgradeTrigger {
public:
    /**
     * @param otaManager  实际干活的 OTA 管理器（本类不负责销毁）
     * @param otaBaseDir  OTA 基础目录（如 /mnt/emmc/ota），inbox 在其下的 fw/sw 子目录
     */
    LocalUpgradeTrigger(ota::OtaManager& otaManager, const std::string& otaBaseDir);

    /**
     * 主循环里周期调用：
     *   有待处理的本地升级 → 扫描对应 inbox 找最新的 .tar.gz/.tgz 包
     *   → 交给 OtaManager；没找到只记日志，不报错
     */
    void poll();

private:
    ota::OtaManager& m_otaManager;
    std::string m_otaBaseDir;  /* OTA 基础目录，inbox 在其下的 fw/sw 子目录 */
};

} // namespace iot_agent
