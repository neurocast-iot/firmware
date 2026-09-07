/**
 * @file p2p_signaling.cpp
 * @brief P2P 信令通道实现（MQTT + cJSON，自 test_p2p_rtc8/P2PSignaling 迁入）
 *
 * 设计要点：
 *   - paho 同步客户端（MQTTClient）+ 异步回调模式（MQTTClient_setCallbacks），
 *     信令消息量小（每会话约 10 条），同步 publish 的阻塞开销可忽略
 *   - SDP 含换行/特殊字符，必须经 cJSON 规范转义后再进入 JSON 载荷
 *   - sid（会话ID）由上层生成并校验，本层透传
 *
 * 相对测试工程新增：relay/alive 两条消息（SRS 降级链路与观看人数追踪）
 */
#include "rtc/p2p_signaling.h"

#include <nc/common/log_utils.h>

#include <cstring>
#include <cstdlib>

#include "cJSON.h"

namespace rtc {

/* MQTT 参数：QoS 1（信令不容丢失）、10s 超时 */
static const int  kQos = 1;
static const long kTimeoutMs = 10000L;

P2PSignaling::P2PSignaling(const std::string& brokerUrl, const std::string& devId,
                           const std::string& role,
                           const std::string& username, const std::string& password)
    : m_brokerUrl(brokerUrl), m_devId(devId), m_role(role),
      m_username(username), m_password(password) {
    m_topicPrefix = "p2p/" + devId;
}

P2PSignaling::~P2PSignaling() {
    disconnect();
}

bool P2PSignaling::connect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_connected) return true;

    /* 断线重连路径：先销毁上次残留的 client（onConnectionLost 只清标志不销毁） */
    if (m_client != nullptr) {
        MQTTClient_destroy(&m_client);
        m_client = nullptr;
    }

    /* clientId 携带角色与设备 ID，避免与业务 MQTT 连接冲突 */
    std::string clientId = "p2p_" + m_role + "_" + m_devId;

    int rc = MQTTClient_create(&m_client, m_brokerUrl.c_str(), clientId.c_str(),
                               MQTTCLIENT_PERSISTENCE_NONE, nullptr);
    if (rc != MQTTCLIENT_SUCCESS) {
        NC_LOGE("[P2PSignaling] MQTTClient_create failed rc={}", rc);
        return false;
    }

    MQTTClient_setCallbacks(m_client, this, onConnectionLost, onMessageArrived, nullptr);

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.keepAliveInterval = 30;
    opts.cleansession = 1;           /* 信令会话无需持久化历史消息 */
    opts.connectTimeout = 10;
    /* MQTT 认证：用户名或密码非空时启用 */
    if (!m_username.empty() || !m_password.empty()) {
        opts.username = const_cast<char*>(m_username.c_str());
        opts.password = const_cast<char*>(m_password.c_str());
    }

    rc = MQTTClient_connect(m_client, &opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        NC_LOGE("[P2PSignaling] connect {} failed rc={}", m_brokerUrl.c_str(), rc);
        MQTTClient_destroy(&m_client);
        m_client = nullptr;
        return false;
    }

    /* 按角色订阅：pusher 收 offer + viewer 候选 + alive 心跳；
     * viewer 收 answer + pusher 候选 + relay 切换通知；bye 双方都收 */
    std::string t1, t2, t4;
    if (m_role == "pusher") {
        t1 = m_topicPrefix + "/offer";
        t2 = m_topicPrefix + "/cand/viewer";
        t4 = m_topicPrefix + "/alive";
    } else {
        t1 = m_topicPrefix + "/answer";
        t2 = m_topicPrefix + "/cand/pusher";
        t4 = m_topicPrefix + "/relay";
    }
    std::string t3 = m_topicPrefix + "/bye";

    if (MQTTClient_subscribe(m_client, t1.c_str(), kQos) != MQTTCLIENT_SUCCESS ||
        MQTTClient_subscribe(m_client, t2.c_str(), kQos) != MQTTCLIENT_SUCCESS ||
        MQTTClient_subscribe(m_client, t3.c_str(), kQos) != MQTTCLIENT_SUCCESS ||
        MQTTClient_subscribe(m_client, t4.c_str(), kQos) != MQTTCLIENT_SUCCESS) {
        NC_LOGE("[P2PSignaling] subscribe failed");
        MQTTClient_disconnect(m_client, 1000);
        MQTTClient_destroy(&m_client);
        m_client = nullptr;
        return false;
    }

    m_connected = true;
    NC_LOGI("[P2PSignaling] connected {} as {} (subscribed: {}, {}, {}, {})",
            m_brokerUrl.c_str(), clientId.c_str(),
            t1.c_str(), t2.c_str(), t3.c_str(), t4.c_str());
    return true;
}

void P2PSignaling::disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_client) {
        if (m_connected) MQTTClient_disconnect(m_client, 1000);
        MQTTClient_destroy(&m_client);
        m_client = nullptr;
    }
    m_connected = false;
}

bool P2PSignaling::isConnected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected;
}

void P2PSignaling::setMessageHandler(MessageHandler handler) {
    m_handler = handler;
}

/* ==================== 发送接口 ==================== */

bool P2PSignaling::sendOffer(const std::string& sid, const std::string& sdp) {
    return publishJson(m_topicPrefix + "/offer", sid, "sdp", sdp);
}

bool P2PSignaling::sendAnswer(const std::string& sid, const std::string& sdp) {
    return publishJson(m_topicPrefix + "/answer", sid, "sdp", sdp);
}

bool P2PSignaling::sendCandidate(const std::string& sid, const std::string& candidate) {
    /* 候选按本端角色发到对应 topic，对端只订阅对方角色的候选 */
    return publishJson(m_topicPrefix + "/cand/" + m_role, sid, "candidate", candidate);
}

bool P2PSignaling::sendBye(const std::string& sid, const std::string& reason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected || m_client == nullptr) {
        NC_LOGW("[P2PSignaling] publish skipped: not connected");
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sid", sid.c_str());
    if (!reason.empty()) {
        cJSON_AddStringToObject(root, "reason", reason.c_str());
    }
    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == nullptr) return false;

    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = (int)strlen(payload);
    msg.qos = kQos;
    msg.retained = 0;

    MQTTClient_deliveryToken token;
    std::string topic = m_topicPrefix + "/bye";
    int rc = MQTTClient_publishMessage(m_client, topic.c_str(), &msg, &token);
    if (rc == MQTTCLIENT_SUCCESS) {
        rc = MQTTClient_waitForCompletion(m_client, token, kTimeoutMs);
    }
    free(payload);

    if (rc != MQTTCLIENT_SUCCESS) {
        NC_LOGE("[P2PSignaling] publish bye failed rc={}", rc);
        return false;
    }
    return true;
}

bool P2PSignaling::sendRelay(const std::string& sid, const std::string& whepUrl) {
    return publishJson(m_topicPrefix + "/relay", sid, "url", whepUrl);
}

bool P2PSignaling::sendAlive(const std::string& sid) {
    return publishJson(m_topicPrefix + "/alive", sid, nullptr, "");
}

bool P2PSignaling::publishJson(const std::string& topic, const std::string& sid,
                               const char* bodyKey, const std::string& bodyVal) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected || m_client == nullptr) {
        NC_LOGW("[P2PSignaling] publish skipped: not connected");
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sid", sid.c_str());
    if (bodyKey != nullptr) {
        cJSON_AddStringToObject(root, bodyKey, bodyVal.c_str());
    }
    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == nullptr) return false;

    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = (int)strlen(payload);
    msg.qos = kQos;
    msg.retained = 0;

    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publishMessage(m_client, topic.c_str(), &msg, &token);
    if (rc == MQTTCLIENT_SUCCESS) {
        rc = MQTTClient_waitForCompletion(m_client, token, kTimeoutMs);
    }
    free(payload);

    if (rc != MQTTCLIENT_SUCCESS) {
        NC_LOGE("[P2PSignaling] publish {} failed rc={}", topic.c_str(), rc);
        return false;
    }
    return true;
}

/* ==================== paho 回调 ==================== */

int P2PSignaling::onMessageArrived(void* ctx, char* topicName, int topicLen,
                                   MQTTClient_message* message) {
    P2PSignaling* self = (P2PSignaling*)ctx;
    (void)topicLen;

    do {
        if (self == nullptr || self->m_handler == nullptr || message == nullptr) break;

        std::string topic(topicName);
        /* 从 topic 提取消息类型：p2p/{devId}/offer|answer|bye|relay|alive|cand/{role} */
        std::string type;
        if (topic == self->m_topicPrefix + "/offer")        type = "offer";
        else if (topic == self->m_topicPrefix + "/answer")  type = "answer";
        else if (topic == self->m_topicPrefix + "/bye")     type = "bye";
        else if (topic == self->m_topicPrefix + "/relay")   type = "relay";
        else if (topic == self->m_topicPrefix + "/alive")   type = "alive";
        else if (topic.rfind(self->m_topicPrefix + "/cand/", 0) == 0) type = "candidate";
        if (type.empty()) break;

        std::string payload((const char*)message->payload, message->payloadlen);
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            NC_LOGE("[P2PSignaling] JSON parse failed: %.128s", payload.c_str());
            break;
        }

        cJSON* jsid = cJSON_GetObjectItem(root, "sid");
        std::string sid = (jsid && cJSON_IsString(jsid)) ? jsid->valuestring : "";

        std::string body;
        if (type == "offer") {
            /* offer 消息传完整 JSON，让上层解析 SDP + mode */
            body = payload;
        } else {
            /* 其余消息按类型取对应字段：candidate→"candidate"，relay→"url"，其余→"sdp" */
            const char* bodyKey = (type == "candidate") ? "candidate"
                                : (type == "relay")     ? "url"
                                                        : "sdp";
            cJSON* jbody = cJSON_GetObjectItem(root, bodyKey);
            if (jbody && cJSON_IsString(jbody)) body = jbody->valuestring;
        }
        cJSON_Delete(root);

        self->m_handler(type, sid, body);
    } while (0);

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;   /* 1=消息已处理（paho 约定） */
}

void P2PSignaling::onConnectionLost(void* ctx, char* cause) {
    P2PSignaling* self = (P2PSignaling*)ctx;
    NC_LOGE("[P2PSignaling] connection lost: {}", cause ? cause : "unknown");
    if (self) {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_connected = false;
    }
}

} // namespace rtc
