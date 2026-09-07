/**
 * @file ota_manager.h
 * @brief OTA 升级编排管理器（作为 iot_agent 内部模块）
 *
 * 设计要点（相对于老项目 ota_agent 独立进程）：
 *   - 砍单例：作为 iot_agent 的成员变量，生命周期由 iot_agent 管理
 *   - 砍 IPC：状态上报直接调 CloudService::publishTelemetry()，不走 IPC
 *   - 砍 exporter 监控：iot_agent 后续自己加统一监控
 *   - 砍 SwFileWatcher：本地升级靠 iot_agent 主循环的信号触发
 *   - 保留 worker 线程 + 队列：下载/升级是长耗时操作，不阻塞 MQTT 回调
 *
 * 升级流程：
 *   1) 收到 OtaNotice（MQTT 或本地信号）→ 入队
 *   2) worker 线程取任务 → 下载 → SHA256 校验 → 执行升级 → 写版本文件
 *   3) 升级成功 → 调 OtaResultReporter::reportState("UPDATED") → fork 子进程延迟 10s 后 reboot
 *   4) 任何失败 → 调 OtaResultReporter::reportState("FAILED", err) → 回滚（SwHandler 内部做）
 */
#pragma once

#include "ota_types.h"  // iot_agent::OtaNotice + OtaPackageType + OTA 模块内部类型
#include "ota_config.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace iot_agent {
class CloudService;  // 前向声明，避免在头文件 include 整个 service
class LocalUpgradeTrigger;  // 前向声明

namespace ota {

class OtaResultReporter;  // 前向声明

/**
 * @brief OTA 编排管理器
 *
 * 生命周期：和 iot_agent 进程同寿命，iot_agent main.cpp 里创建并调 initialize/stop。
 * 线程模型：收到通知后直接启动异步线程处理，正在升级时忽略新通知。
 */
class OtaManager {
public:
    OtaManager();
    ~OtaManager();

    OtaManager(const OtaManager&) = delete;
    OtaManager& operator=(const OtaManager&) = delete;

    /**
     * @brief 初始化
     * @param cloud CloudService 指针（用于状态上报；传 nullptr 时跳过上报）
     * @param base_dir OTA 根目录，默认 /mnt/emmc/ota
     */
    bool initialize(CloudService* cloud, const std::string& base_dir = OtaPaths::OTA_BASE);

    /** MQTT 收到 OTA 通知时调用（CloudService 的 onOta 回调） */
    void onOtaNotice(const OtaNotice& notice);

    /**
     * @brief 本地升级入口（信号触发）
     * @param package_path 已通过 SCP 上传到设备的升级包路径
     * @param type "fw" 或 "sw"
     * @param version 版本号（用于状态上报；空则从文件名解析）
     * @return true=升级已触发
     */
    bool upgradeFromLocalFile(const std::string& package_path,
                              const std::string& type,
                              const std::string& version);

    /**
     * @brief 本地升级轮询（主循环调用）
     * 检查 SignalManager 是否有待处理的本地升级信号，有则扫描 inbox 并触发升级
     */
    void pollLocalUpgrade();

    void stop();
    bool isBusy() const;

private:
    // 内部使用的完整通知结构（包含 type/title/version/url/checksum/...）
    struct Notice {
        OtaPackageType type = OtaPackageType::FIRMWARE;
        OtaSource source = OtaSource::REMOTE;  // 升级来源（远程/本地）
        std::string title;
        std::string version;
        std::string url;
        std::string checksum;
        std::string checksum_algorithm;
        uint64_t size = 0;
    };

    void processNotice(Notice notice);

    // 核心升级执行（被 MQTT 流程和本地升级共用）
    bool executeUpgradeCore(const Notice& notice, const std::string& pkgPath, bool skip_version_dedup);

    // 下载升级包
    bool downloadPackage(const Notice& notice, const std::string& outDir,
                         std::string& outPath, std::string& outErr);
    // SHA256 校验
    bool verifyPackageChecksum(const Notice& notice, const std::string& filePath, std::string& outErr);

    // 状态上报：委托给 OtaResultReporter
    void reportState(const Notice& notice, const char* state, const std::string& err = "");

    // 工具
    std::string currentVersionPath(const Notice& notice) const;
    std::string readInstalledVersion(const Notice& notice) const;
    std::string packageDir(const Notice& notice) const;
    bool writeTextAtomic(const std::string& path, const std::string& content) const;

    // 触发延迟重启（fork 子进程，sleep 10s 后 reboot，不阻塞当前 worker）
    void scheduleReboot();

    CloudService* m_cloud = nullptr;
    std::unique_ptr<OtaResultReporter> m_reporter;  // OTA 状态上报器
    std::unique_ptr<LocalUpgradeTrigger> m_localTrigger;  // 本地升级触发器
    std::string m_base_dir;

    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_stop{false};
    std::mutex m_mu;
    std::unique_ptr<std::thread> m_worker;  // 异步升级线程
};

} // namespace ota
} // namespace iot_agent
