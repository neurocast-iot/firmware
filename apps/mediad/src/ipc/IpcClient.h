/**
 * @file IpcClient.h
 * @brief mediad 的 IPC 通道：DEALER 模式连接 iot_agent
 *
 * 只有一条通道：DEALER 连接到 iot_agent 的 ROUTER
 *   - 接收来自 iot_agent 的命令（如 SNAPSHOT/GET_STATUS），交给 CommandRouter 处理
 *   - 通过 ConfigSyncClient 接收配置更新，自动回复 ACK
 *   - 发送事件给 iot_agent（如 FILE_READY / STATE_CHANGED）
 *
 * 不需要 REQ/REP 和 PUB：所有通信都通过 iot_agent 中转
 */
#pragma once

#include "ipc/MediadMessages.h"

#include "libmq/message_manager.h"
#include "nc/config_sync/config_sync_client.h"

#include <functional>
#include <memory>
#include <string>

namespace mediad {

class CommandRouter;

class IpcClient {
public:
    /**
     * 配置应用回调
     * @param version 配置版本
     * @param config 配置内容，JSON 字符串
     * @return true=应用成功，false=应用失败
     */
    using ConfigApplyCallback = std::function<bool(uint64_t version, const std::string& config)>;

    explicit IpcClient(CommandRouter& router) : m_router(router) {}
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    /**
     * 启动 DEALER 连接
     * @param iotAgentEndpoint iot_agent 的 ROUTER 端点（ipc:///tmp/iot_agent.ipc）
     * @return false=连接失败
     */
    bool start(const std::string& iotAgentEndpoint);

    /** 停止连接（幂等） */
    void stop();

    /**
     * 发送事件给 iot_agent
     * @param event   事件类型（见 message_types.h 的 IpcMessageType::MEDIA_* 枚举）
     * @param payload JSON 载荷文本
     */
    void sendEvent(libmq::IpcMessageType event, const std::string& payload);

    /**
     * 设置配置应用回调
     * 
     * 必须在 start() 之后调用。
     * 收到 CONFIG_SYNC_UPDATE 时会调用此回调应用配置，并根据返回值回复 ACK。
     */
    void setConfigApplyCallback(ConfigApplyCallback callback);

    /**
     * 通知 iot_agent：mediad 启动完成
     * 
     * @param currentConfigVersion 当前配置版本（0=无配置）
     * 
     * 调用时机：所有服务初始化完成后。
     */
    void notifyStarted(uint64_t currentConfigVersion);

    /** 获取底层 MessageManager（供外部订阅其他消息） */
    libmq::MessageManager* messageManager() { return m_iotAgentConn.get(); }

private:
    void onIotAgentMessage(const libmq::MessageHeader& header, const std::string& payload);

    CommandRouter& m_router;
    std::unique_ptr<libmq::MessageManager> m_iotAgentConn;
    std::unique_ptr<nc::config_sync::ConfigSyncClient> m_configSyncClient;
};

} // namespace mediad
