/**
 * @file config_sync_client.h
 * @brief 配置同步客户端（服务侧使用）
 *
 * 各服务（mediad、ota_agent、monitor 等）使用此客户端：
 * 1. 启动完成后调用 notifyStarted() 通知 iot_agent
 * 2. 订阅配置更新，收到后自动回复 ACK
 * 3. 可选：主动请求当前配置
 *
 * 使用示例：
 * @code
 * // 初始化
 * auto client = std::make_unique<nc::config_sync::ConfigSyncClient>(ipcManager, "mediad");
 *
 * // 启动完成后
 * client->notifyStarted(currentConfigVersion);
 *
 * // 订阅配置更新
 * client->onConfigUpdate([](uint64_t version, const std::string& config) {
 *     bool ok = applyConfig(config);
 *     return ok;  // 返回 true=成功，false=失败
 * });
 * @endcode
 */
#pragma once

#include "libmq/message_manager.h"

#include <functional>
#include <memory>
#include <string>

namespace nc {
namespace config_sync {

class ConfigSyncClient {
public:
    /**
     * 配置应用回调
     *
     * @param version 配置版本
     * @param config 配置内容，JSON 字符串
     * @return true=应用成功，false=应用失败
     *
     * 返回 true 时自动回复 CONFIG_SYNC_ACK(success=true)
     * 返回 false 时自动回复 CONFIG_SYNC_ACK(success=false)
     */
    using ConfigApplyCallback = std::function<bool(uint64_t version, const std::string& config)>;

    /**
     * 构造函数
     *
     * @param manager IPC 消息管理器（已启动）
     * @param serviceName 当前服务名字，如 "mediad"
     */
    ConfigSyncClient(libmq::MessageManager& manager, const std::string& serviceName);
    ~ConfigSyncClient();

    ConfigSyncClient(const ConfigSyncClient&) = delete;
    ConfigSyncClient& operator=(const ConfigSyncClient&) = delete;

    /**
     * 通知 iot_agent：服务已启动完成
     *
     * @param currentConfigVersion 当前持有的配置版本（0=无配置）
     *
     * 调用时机：服务所有组件初始化完成后。
     * iot_agent 收到后会：
     * - 标记服务在线
     * - 如果 currentConfigVersion < 最新版本，主动下发最新配置
     */
    void notifyStarted(uint64_t currentConfigVersion);

    /**
     * 订阅配置更新
     *
     * @param callback 配置应用回调
     *
     * 收到 CONFIG_SYNC_UPDATE 时：
     * 1. 调用 callback 应用配置
     * 2. 根据返回值发送 CONFIG_SYNC_ACK
     */
    void onConfigUpdate(ConfigApplyCallback callback);

    /**
     * 主动请求当前配置
     *
     * @param callback 收到配置后的回调
     *
     * 使用场景：服务启动时发现自己没有配置（configVersion=0），
     * 可以主动请求 iot_agent 下发最新配置。
     */
    void requestConfig(std::function<void(uint64_t version, const std::string& config)> callback);

    /**
     * 获取当前服务名字
     */
    const std::string& serviceName() const { return m_serviceName; }

private:
    libmq::MessageManager& m_manager;
    std::string m_serviceName;
    ConfigApplyCallback m_applyCallback;
    std::function<void(uint64_t, const std::string&)> m_requestCallback;
};

} // namespace config_sync
} // namespace nc
