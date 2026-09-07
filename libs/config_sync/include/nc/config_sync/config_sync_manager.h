/**
 * @file config_sync_manager.h
 * @brief 配置同步管理器（iot_agent 侧使用）
 *
 * iot_agent 使用此管理器：
 * 1. 注册需要管理的服务（mediad、ota_agent、monitor 等）
 * 2. 跟踪各服务在线状态和配置版本
 * 3. 下发配置时自动重试，直到收到 ACK
 * 4. 服务启动时主动下发最新配置
 *
 * 使用示例：
 * @code
 * // 初始化
 * auto manager = std::make_unique<nc::config_sync::ConfigSyncManager>(ipcManager);
 * manager->registerService("mediad");
 * manager->registerService("ota_agent");
 *
 * // 收到云端配置时
 * manager->broadcastConfig(version, config);
 * @endcode
 */
#pragma once

#include "libmq/message_manager.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nc {
namespace config_sync {

/**
 * 服务状态
 */
struct ServiceState {
    std::string name;           // 服务名字
    bool online = false;        // 是否在线
    uint64_t configVersion = 0; // 当前持有的配置版本
    std::string pendingConfig;  // 待下发的配置（服务离线时缓存）
    uint64_t pendingVersion = 0;// 待下发配置的版本
    int retryCount = 0;         // 重试次数
};

class ConfigSyncManager {
public:
    /**
     * 配置下发结果回调
     *
     * @param serviceName 服务名字
     * @param version 配置版本
     * @param success 是否成功
     */
    using ConfigPushResultCallback = std::function<void(const std::string& serviceName,
                                                         uint64_t version,
                                                         bool success)>;

    /**
     * 构造函数
     *
     * @param manager IPC 消息管理器（已启动）
     */
    explicit ConfigSyncManager(libmq::MessageManager& manager);
    ~ConfigSyncManager();

    ConfigSyncManager(const ConfigSyncManager&) = delete;
    ConfigSyncManager& operator=(const ConfigSyncManager&) = delete;

    /**
     * 注册服务
     *
     * @param serviceName 服务名字，如 "mediad"
     *
     * 注册后才能跟踪该服务的状态。
     */
    void registerService(const std::string& serviceName);

    /**
     * 设置当前最新配置
     *
     * @param version 配置版本（last_updated_time）
     * @param config 配置内容，JSON 字符串
     *
     * 调用后会自动向所有在线服务下发配置。
     * 离线服务的配置会被缓存，等服务上线后下发。
     */
    void setCurrentConfig(uint64_t version, const std::string& config);

    /**
     * 获取当前最新配置
     */
    uint64_t currentVersion() const { return m_currentVersion; }
    const std::string& currentConfig() const { return m_currentConfig; }

    /**
     * 获取服务状态
     */
    bool isServiceOnline(const std::string& serviceName) const;
    uint64_t getServiceConfigVersion(const std::string& serviceName) const;

    /**
     * 向指定服务下发配置
     *
     * @param serviceName 目标服务
     * @param callback 下发结果回调（可选）
     *
     * 如果服务离线，配置会被缓存，等服务上线后自动下发。
     * 如果发送失败，会自动重试（最多 3 次，间隔 1s/2s/4s）。
     */
    void pushConfig(const std::string& serviceName, ConfigPushResultCallback callback = nullptr);

    /**
     * 向所有在线服务广播配置
     *
     * @param callback 下发结果回调（可选，每个服务完成时都会调用）
     */
    void broadcastConfig(ConfigPushResultCallback callback = nullptr);

    /**
     * 标记服务在线（收到心跳后调用）
     *
     * @param serviceName 服务名字
     *
     * 标记后会检查是否需要下发缓存的配置。
     */
    void markServiceOnline(const std::string& serviceName);

private:
    void onServiceStarted(const libmq::MessageHeader& header, const std::string& payload);
    void onConfigAck(const libmq::MessageHeader& header, const std::string& payload);
    void onConfigRequest(const libmq::MessageHeader& header, const std::string& payload);

    void doPushConfig(const std::string& serviceName, int retryCount, ConfigPushResultCallback callback);
    void scheduleRetry(const std::string& serviceName, int retryCount, ConfigPushResultCallback callback);

    libmq::MessageManager& m_manager;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ServiceState> m_services;

    uint64_t m_currentVersion = 0;
    std::string m_currentConfig;
};

} // namespace config_sync
} // namespace nc
