/**
 * @file ipc_hub.cpp
 * @brief IPC 中心节点封装实现
 */
#include "ipc_hub.h"

#include "config/cloud_config_translator.h"  /* translateCloudToMediad */
#include "libmq/message_types.h"
#include "nc/common/log_utils.h"

#include "cJSON.h"

#include <ctime>

namespace iot_agent {

IpcHub::~IpcHub() {
    stop();
}

bool IpcHub::start() {
    /* lib-mq 日志接入 nc 日志系统 */
    auto mqLog = [](libmq::LogLevel level, const std::string& msg) {
        switch (level) {
            case libmq::LogLevel::Error: NC_LOGE("[mq] {}", msg.c_str()); break;
            case libmq::LogLevel::Warn:  NC_LOGW("[mq] {}", msg.c_str()); break;
            case libmq::LogLevel::Debug: NC_LOGD("[mq] {}", msg.c_str()); break;
            default:                     NC_LOGI("[mq] {}", msg.c_str()); break;
        }
    };

    m_manager = libmq::MessageManager::builder()
        .inprocEndpoint("inproc://iot_agent_hub")
        .ipcEndpoint("ipc:///tmp/iot_agent.ipc")
        .asRouter("iot_agent")
        .logCallback(mqLog)
        .build();
    if (!m_manager) {
        NC_LOGW("[IpcHub] build failed, running without IPC forwarding");
        return false;
    }

    libmq::MqResult ret = m_manager->start();
    if (!ret.ok) {
        NC_LOGW("[IpcHub] start failed: {}, running without IPC forwarding", ret.message.c_str());
        m_manager.reset();
        return false;
    }
    NC_LOGI("[IpcHub] started, bound to ipc:///tmp/iot_agent.ipc (ROUTER)");

    /* 配置同步管理器：跟踪各服务在线状态，保证配置可靠下发 */
    m_configSync = std::make_unique<nc::config_sync::ConfigSyncManager>(*m_manager);
    m_configSync->registerService("mediad");
    /* TODO: 注册其他服务 */
    // m_configSync->registerService("ota_agent");
    // m_configSync->registerService("monitor");

    /* 订阅心跳确认：服务收到心跳请求后发送 ACK，这里标记服务在线 */
    m_manager->subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_HEARTBEAT_ACK),
        [this](const libmq::MessageHeader& /*header*/, const std::string& payload) {
            NC_LOGI("[IpcHub] received heartbeat from service");
            cJSON* root = cJSON_Parse(payload.c_str());
            if (root) {
                cJSON* nameItem = cJSON_GetObjectItem(root, "serviceName");
                if (cJSON_IsString(nameItem) && m_configSync) {
                    m_configSync->markServiceOnline(nameItem->valuestring);
                }
                cJSON_Delete(root);
            }
        });

    /* 广播心跳请求：所有服务收到后回应 ACK */
    NC_LOGI("[IpcHub] broadcasting heartbeat request");
    m_manager->send(static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_HEARTBEAT_REQUEST), "{}");
    return true;
}

void IpcHub::stop() {
    if (m_manager) {
        m_manager->stop();
        m_manager.reset();
    }
    m_configSync.reset();
}

bool IpcHub::sendTo(const std::string& target, uint32_t type, const std::string& payload) {
    if (!m_manager) return false;
    return m_manager->sendTo(target, type, payload).ok;
}

bool IpcHub::publishConfigDelta(uint64_t version, const std::string& delta) {
    NC_LOGI("[IpcHub] config delta from cloud: {}", delta.c_str());

    /* 翻译云端字段名为 mediad 的配置结构 */
    std::string mediadDelta = translateCloudToMediad(delta);

    /* 注入 device_id：云端不下发这个字段，但 mediad 需要它拼 WHIP URL */
    if (!m_deviceId.empty()) {
        cJSON* root = cJSON_Parse(mediadDelta.c_str());
        if (!root) root = cJSON_CreateObject();
        cJSON_DeleteItemFromObject(root, "device_id");
        cJSON_AddStringToObject(root, "device_id", m_deviceId.c_str());
        char* updated = cJSON_PrintUnformatted(root);
        if (updated) {
            mediadDelta = updated;
            free(updated);
        }
        cJSON_Delete(root);
    }
    NC_LOGI("[IpcHub] translated for mediad: {}", mediadDelta.c_str());

    if (!m_configSync) {
        /* IPC 没启动，缓存不了也没法发，只能记日志 */
        NC_LOGW("[IpcHub] ConfigSyncManager not available, config dropped");
        return false;
    }

    /* 设置当前配置，ConfigSyncManager 会自动向所有在线服务下发 */
    m_configSync->setCurrentConfig(version, mediadDelta);
    NC_LOGI("[IpcHub] config set to ConfigSyncManager, version={}", version);
    return true;
}

} // namespace iot_agent
