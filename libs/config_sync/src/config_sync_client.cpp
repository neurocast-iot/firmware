/**
 * @file config_sync_client.cpp
 * @brief 配置同步客户端实现（服务侧）
 */
#include "nc/common/log_utils.h"

#include "nc/config_sync/config_sync_client.h"
#include "nc/config_sync/config_sync_types.h"

#include "libmq/message_types.h"

namespace nc {
namespace config_sync {

ConfigSyncClient::ConfigSyncClient(libmq::MessageManager& manager, const std::string& serviceName)
    : m_manager(manager)
    , m_serviceName(serviceName) {

    /* 订阅配置更新消息 */
    m_manager.subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_UPDATE),
        [this](const libmq::MessageHeader& header, const std::string& payload) {
            /* 检查是否是发给自己的 */
            if (std::string(header.to) != m_serviceName && header.to[0] != '\0') {
                return;
            }

            ConfigUpdateMsg msg = ConfigUpdateMsg::fromJson(payload);
            NC_LOGI("[ConfigSync] received config update: version={}", msg.version);

            if (m_applyCallback) {
                bool success = m_applyCallback(msg.version, msg.config);
                /* 回复 ACK */
                ConfigAckMsg ack;
                ack.version = msg.version;
                ack.success = success;
                if (!success) {
                    ack.error = "apply config failed";
                }
                m_manager.sendIpc(
                    static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_ACK),
                    ack.toJson());
                NC_LOGI("[ConfigSync] sent ACK: version={}, success={}", ack.version, ack.success);
            }
        });

    /* 订阅配置响应消息（主动请求配置时的回复） */
    m_manager.subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_RESPONSE),
        [this](const libmq::MessageHeader& header, const std::string& payload) {
            /* 检查是否是发给自己的 */
            if (std::string(header.to) != m_serviceName && header.to[0] != '\0') {
                return;
            }

            ConfigResponseMsg msg = ConfigResponseMsg::fromJson(payload);
            NC_LOGI("[ConfigSync] received config response: version={}", msg.version);

            if (m_requestCallback) {
                m_requestCallback(msg.version, msg.config);
            }
        });

    /* 订阅心跳请求消息（iot_agent 重启后会广播请求所有服务发送心跳） */
    m_manager.subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_HEARTBEAT_REQUEST),
        [this](const libmq::MessageHeader& header, const std::string& payload) {
            NC_LOGI("[ConfigSync] received heartbeat request, sending heartbeat ACK");
            /* 发送心跳确认，让 iot_agent 知道我还在线 */
            m_manager.sendIpc(
                static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_HEARTBEAT_ACK),
                "{\"serviceName\":\"" + m_serviceName + "\"}");
        });
}

ConfigSyncClient::~ConfigSyncClient() = default;

void ConfigSyncClient::notifyStarted(uint64_t currentConfigVersion) {
    ServiceStartedMsg msg;
    msg.serviceName = m_serviceName;
    msg.configVersion = currentConfigVersion;

    NC_LOGI("[ConfigSync] notifyStarted: service={}, configVersion={}", 
             m_serviceName.c_str(), currentConfigVersion);

    m_manager.sendIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_STARTED),
        msg.toJson());
}

void ConfigSyncClient::onConfigUpdate(ConfigApplyCallback callback) {
    m_applyCallback = std::move(callback);
}

void ConfigSyncClient::requestConfig(std::function<void(uint64_t, const std::string&)> callback) {
    m_requestCallback = std::move(callback);

    ConfigRequestMsg msg;
    msg.serviceName = m_serviceName;

    NC_LOGI("[ConfigSync] requestConfig: service={}", m_serviceName.c_str());

    m_manager.sendIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_REQUEST),
        msg.toJson());
}

} // namespace config_sync
} // namespace nc
