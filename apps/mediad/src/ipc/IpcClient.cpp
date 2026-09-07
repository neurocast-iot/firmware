/**
 * @file IpcClient.cpp
 * @brief mediad 的 IPC 通道实现：DEALER 模式连接 iot_agent
 */
#include "ipc/IpcClient.h"
#include "ipc/CommandRouter.h"

#include "nc/common/log_utils.h"

namespace mediad {

namespace {

/* lib-mq 内部日志接入 nc 日志系统 */
void mqLog(libmq::LogLevel level, const std::string& msg) {
    switch (level) {
        case libmq::LogLevel::Error: NC_LOGE("[mq] {}", msg.c_str()); break;
        case libmq::LogLevel::Warn:  NC_LOGW("[mq] {}", msg.c_str()); break;
        case libmq::LogLevel::Debug: NC_LOGD("[mq] {}", msg.c_str()); break;
        default:                     NC_LOGI("[mq] {}", msg.c_str()); break;
    }
}

} // namespace

IpcClient::~IpcClient() {
    stop();
}

bool IpcClient::start(const std::string& iotAgentEndpoint) {
    if (iotAgentEndpoint.empty()) {
        NC_LOGW("IpcClient: iotAgentEndpoint is empty, IPC disabled");
        return false;
    }

    /* DEALER 模式连接到 iot_agent 的 ROUTER */
    m_iotAgentConn = libmq::MessageManager::builder()
                         .inprocEndpoint("inproc://mediad_iot")
                         .ipcEndpoint(iotAgentEndpoint)
                         .asDealer("mediad")
                         .logCallback(mqLog)
                         .build();
    if (!m_iotAgentConn) {
        NC_LOGE("IpcClient: build DEALER connection failed");
        return false;
    }

    /* 创建配置同步客户端 */
    m_configSyncClient = std::make_unique<nc::config_sync::ConfigSyncClient>(
        *m_iotAgentConn, "mediad");

    /* 订阅来自 iot_agent 的命令：
     * - CONFIG_UPDATE 由 ConfigSyncClient 处理（配置更新 + ACK）
     * - Command 枚举（12000-12999）路由到 CommandRouter 处理
     *   （iot_agent 通过 sendTo("mediad") 发的 RPC 翻译命令，如 MEDIA_STREAM_START） */
    m_iotAgentConn->subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_UPDATE),
        [this](const libmq::MessageHeader& h, const std::string& p) {
            onIotAgentMessage(h, p);
        });

    /* 订阅 mediad 命令（iot_agent RPC 翻译后通过 sendTo 发过来） */
    static const uint32_t s_commands[] = {
        static_cast<uint32_t>(Command::MEDIA_SNAPSHOT),
        static_cast<uint32_t>(Command::MEDIA_STREAM_START),
        static_cast<uint32_t>(Command::MEDIA_STREAM_STOP),
        static_cast<uint32_t>(Command::MEDIA_GET_STATUS),
        static_cast<uint32_t>(Command::MEDIA_GET_CONFIG),
    };
    for (uint32_t cmd : s_commands) {
        m_iotAgentConn->subscribeIpc(cmd,
            [this](const libmq::MessageHeader& h, const std::string& p) {
                onIotAgentMessage(h, p);
            });
    }

    libmq::MqResult ret = m_iotAgentConn->start();
    if (!ret.ok) {
        NC_LOGE("IpcClient: start DEALER connection failed: {}", ret.message.c_str());
        m_iotAgentConn.reset();
        return false;
    }

    NC_LOGI("IpcClient: connected to iot_agent at {}", iotAgentEndpoint.c_str());
    return true;
}

void IpcClient::stop() {
    if (m_iotAgentConn) {
        m_iotAgentConn->stop();
        m_iotAgentConn.reset();
    }
}

void IpcClient::sendEvent(libmq::IpcMessageType event, const std::string& payload) {
    if (!m_iotAgentConn) {
        NC_LOGW("IpcClient: sendEvent dropped (type={}), iot_agent not connected", static_cast<uint32_t>(event));
        return;
    }
    libmq::MqResult ret = m_iotAgentConn->sendIpc(static_cast<uint32_t>(event), payload);
    if (!ret.ok) {
        NC_LOGW("IpcClient: send event {} failed: {}", static_cast<uint32_t>(event), ret.message.c_str());
    } else {
        NC_LOGI("IpcClient: sent event {} ok, payload len={}", static_cast<uint32_t>(event), payload.size());
    }
}

void IpcClient::setConfigApplyCallback(ConfigApplyCallback callback) {
    if (m_configSyncClient) {
        m_configSyncClient->onConfigUpdate(std::move(callback));
    } else {
        NC_LOGW("IpcClient: setConfigApplyCallback called before start()");
    }
}

void IpcClient::notifyStarted(uint64_t currentConfigVersion) {
    if (m_configSyncClient) {
        m_configSyncClient->notifyStarted(currentConfigVersion);
    } else {
        NC_LOGW("IpcClient: notifyStarted called before start()");
    }
}

void IpcClient::onIotAgentMessage(const libmq::MessageHeader& header, const std::string& payload) {
    NC_LOGI("IpcClient: received from iot_agent: type={} from={} len={}",
            header.type, header.from, payload.size());
    
    /* IPC 消息已经知道类型（在 header.type 里），但 dispatch 期望信封格式
     * {"type":命令号,"payload":{...}}，所以要把 payload 包一层再传进去 */
    std::string envelope = "{\"type\":" + std::to_string(header.type) +
                           ",\"payload\":" + payload + "}";
    std::string response = m_router.dispatch(envelope);
    NC_LOGI("IpcClient: dispatch response: {}", response.c_str());
}

} // namespace mediad
