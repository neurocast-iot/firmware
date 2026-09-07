/**
 * @file config_sync_manager.cpp
 * @brief 配置同步管理器实现（iot_agent 侧）
 */
#include "nc/common/log_utils.h"

#include "nc/config_sync/config_sync_manager.h"
#include "nc/config_sync/config_sync_types.h"

#include "libmq/message_types.h"

#include <chrono>
#include <thread>
#include <vector>

namespace nc {
namespace config_sync {

ConfigSyncManager::ConfigSyncManager(libmq::MessageManager& manager)
    : m_manager(manager) {

    /* 订阅服务启动消息 */
    m_manager.subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_STARTED),
        [this](const libmq::MessageHeader& header, const std::string& payload) {
            onServiceStarted(header, payload);
        });

    /* 订阅配置确认消息 */
    m_manager.subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_ACK),
        [this](const libmq::MessageHeader& header, const std::string& payload) {
            onConfigAck(header, payload);
        });

    /* 订阅配置请求消息 */
    m_manager.subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_REQUEST),
        [this](const libmq::MessageHeader& header, const std::string& payload) {
            onConfigRequest(header, payload);
        });
}

ConfigSyncManager::~ConfigSyncManager() = default;

void ConfigSyncManager::registerService(const std::string& serviceName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ServiceState state;
    state.name = serviceName;
    m_services[serviceName] = state;
    NC_LOGI("[ConfigSync] registered service: {}", serviceName.c_str());
}

void ConfigSyncManager::setCurrentConfig(uint64_t version, const std::string& config) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_currentVersion = version;
        m_currentConfig = config;
    }
    NC_LOGI("[ConfigSync] setCurrentConfig: version={}", version);

    /* 自动向所有在线服务下发 */
    broadcastConfig();
}

bool ConfigSyncManager::isServiceOnline(const std::string& serviceName) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_services.find(serviceName);
    if (it == m_services.end()) {
        return false;
    }
    return it->second.online;
}

uint64_t ConfigSyncManager::getServiceConfigVersion(const std::string& serviceName) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_services.find(serviceName);
    if (it == m_services.end()) {
        return 0;
    }
    return it->second.configVersion;
}

void ConfigSyncManager::pushConfig(const std::string& serviceName, ConfigPushResultCallback callback) {
    bool isOnline = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_services.find(serviceName);
        if (it == m_services.end()) {
            NC_LOGW("[ConfigSync] pushConfig: unknown service '{}'", serviceName.c_str());
            if (callback) {
                callback(serviceName, 0, false);
            }
            return;
        }

        if (!it->second.online) {
            /* 服务离线，缓存配置 */
            it->second.pendingConfig = m_currentConfig;
            it->second.pendingVersion = m_currentVersion;
            NC_LOGI("[ConfigSync] pushConfig: service '{}' offline, config cached", serviceName.c_str());
            if (callback) {
                callback(serviceName, m_currentVersion, false);
            }
            return;
        }
        isOnline = true;
    }
    /* 解锁后下发，避免死锁 */
    if (isOnline) {
        doPushConfig(serviceName, 0, callback);
    }
}

void ConfigSyncManager::broadcastConfig(ConfigPushResultCallback callback) {
    /* 收集在线服务名字，避免在锁内下发 */
    std::vector<std::string> onlineServices;
    std::vector<std::string> offlineServices;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& kv : m_services) {
            if (kv.second.online) {
                onlineServices.push_back(kv.first);
            } else {
                offlineServices.push_back(kv.first);
            }
        }
    }
    /* 解锁后下发，避免死锁 */
    for (const auto& name : onlineServices) {
        pushConfig(name, callback);
    }
    /* 记录离线服务，配置已缓存，等服务上线后下发 */
    for (const auto& name : offlineServices) {
        NC_LOGI("[ConfigSync] service '{}' offline, config cached, will send on startup", name.c_str());
    }
    if (onlineServices.empty() && !offlineServices.empty()) {
        NC_LOGI("[ConfigSync] no services online, config saved for later");
    }
}

void ConfigSyncManager::onServiceStarted(const libmq::MessageHeader& header, const std::string& payload) {
    ServiceStartedMsg msg = ServiceStartedMsg::fromJson(payload);
    NC_LOGI("[ConfigSync] service started: name={}, configVersion={}", 
             msg.serviceName.c_str(), msg.configVersion);

    bool needPush = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_services.find(msg.serviceName);
        if (it == m_services.end()) {
            NC_LOGW("[ConfigSync] unknown service '{}', ignored", msg.serviceName.c_str());
            return;
        }

        /* 标记服务在线 */
        it->second.online = true;
        it->second.configVersion = msg.configVersion;
        it->second.retryCount = 0;

        /* 检查是否需要下发配置 */
        if (m_currentVersion > msg.configVersion) {
            NC_LOGI("[ConfigSync] service '{}' needs config update: {} -> {}", 
                     msg.serviceName.c_str(), msg.configVersion, m_currentVersion);
            needPush = true;
        }
    }
    /* 解锁后下发，避免死锁 */
    if (needPush) {
        pushConfig(msg.serviceName, nullptr);
    }
}

void ConfigSyncManager::markServiceOnline(const std::string& serviceName) {
    bool needPush = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_services.find(serviceName);
        if (it == m_services.end()) {
            NC_LOGW("[ConfigSync] markServiceOnline: unknown service '{}'", serviceName.c_str());
            return;
        }
        if (it->second.online) {
            /* 已经在线，不需要重复标记 */
            return;
        }
        it->second.online = true;
        it->second.retryCount = 0;
        NC_LOGI("[ConfigSync] service '{}' marked online via heartbeat", serviceName.c_str());
        /* 检查是否需要下发配置 */
        if (m_currentVersion > it->second.configVersion) {
            needPush = true;
        }
    }
    if (needPush) {
        pushConfig(serviceName, nullptr);
    }
}

void ConfigSyncManager::onConfigAck(const libmq::MessageHeader& header, const std::string& payload) {
    ConfigAckMsg msg = ConfigAckMsg::fromJson(payload);
    NC_LOGI("[ConfigSync] config ACK: from={}, version={}, success={}", 
             header.from, msg.version, msg.success);

    std::lock_guard<std::mutex> lock(m_mutex);
    /* 找到发送 ACK 的服务 */
    std::string serviceName(header.from);
    auto it = m_services.find(serviceName);
    if (it == m_services.end()) {
        NC_LOGW("[ConfigSync] ACK from unknown service '{}'", serviceName.c_str());
        return;
    }

    if (msg.success) {
        /* 更新服务配置版本 */
        it->second.configVersion = msg.version;
        it->second.retryCount = 0;
        it->second.pendingConfig.clear();
        it->second.pendingVersion = 0;
    } else {
        NC_LOGW("[ConfigSync] config apply failed for '{}': {}", 
                 serviceName.c_str(), msg.error.c_str());
    }
}

void ConfigSyncManager::onConfigRequest(const libmq::MessageHeader& header, const std::string& payload) {
    ConfigRequestMsg msg = ConfigRequestMsg::fromJson(payload);
    NC_LOGI("[ConfigSync] config request from: {}", msg.serviceName.c_str());

    std::string payloadStr;
    uint64_t version;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_services.find(msg.serviceName);
        if (it == m_services.end()) {
            NC_LOGW("[ConfigSync] request from unknown service '{}'", msg.serviceName.c_str());
            return;
        }

        /* 回复当前配置 */
        ConfigResponseMsg response;
        response.version = m_currentVersion;
        response.config = m_currentConfig;
        payloadStr = response.toJson();
        version = m_currentVersion;
    }
    /* 解锁后发送，避免死锁 */
    m_manager.sendTo(msg.serviceName,
                     static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_RESPONSE),
                     payloadStr);
    NC_LOGI("[ConfigSync] sent config response to '{}': version={}", 
             msg.serviceName.c_str(), version);
}

void ConfigSyncManager::doPushConfig(const std::string& serviceName, int retryCount, ConfigPushResultCallback callback) {
    ConfigUpdateMsg msg;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        msg.version = m_currentVersion;
        msg.config = m_currentConfig;
    }

    NC_LOGI("[ConfigSync] pushing config to '{}': version={}, retry={}", 
             serviceName.c_str(), msg.version, retryCount);

    /* 构造发送消息，指定目标服务 */
    std::string payloadStr = msg.toJson();
    libmq::MessageHeader hdr{};
    hdr.type = static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_SYNC_UPDATE);
    std::strncpy(hdr.to, serviceName.c_str(), libmq::MessageHeader::kNameMaxLen - 1);

    libmq::MqResult ret = m_manager.sendTo(serviceName, hdr.type, payloadStr);
    if (!ret.ok) {
        NC_LOGW("[ConfigSync] push config to '{}' failed: {}", serviceName.c_str(), ret.message.c_str());
        if (retryCount < 3) {
            scheduleRetry(serviceName, retryCount + 1, callback);
        } else {
            NC_LOGE("[ConfigSync] push config to '{}' failed after 3 retries", serviceName.c_str());
            if (callback) {
                callback(serviceName, msg.version, false);
            }
        }
        return;
    }

    NC_LOGI("[ConfigSync] push config to '{}' ok, waiting for ACK", serviceName.c_str());
    /* 注意：这里不立即调用 callback，等收到 ACK 后再调用 */
}

void ConfigSyncManager::scheduleRetry(const std::string& serviceName, int retryCount, ConfigPushResultCallback callback) {
    /* 指数退避：1s, 2s, 4s */
    int delaySec = 1 << (retryCount - 1);
    NC_LOGI("[ConfigSync] scheduling retry for '{}' in {}s (retry={})", 
             serviceName.c_str(), delaySec, retryCount);

    std::thread([this, serviceName, retryCount, callback, delaySec]() {
        std::this_thread::sleep_for(std::chrono::seconds(delaySec));
        doPushConfig(serviceName, retryCount, callback);
    }).detach();
}

} // namespace config_sync
} // namespace nc
