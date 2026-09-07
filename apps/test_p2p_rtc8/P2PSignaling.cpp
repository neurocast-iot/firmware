/**
 * P2PSignaling.cpp - P2P 信令通道实现（MQTT + cJSON）
 *
 * 设计要点：
 *   - paho 同步客户端（MQTTClient）+ 异步回调模式（MQTTClient_setCallbacks），
 *     与 mqtt-starter/mqtt_client 相同的用法，信令消息量小（每会话约 10 条），
 *     同步 publish 的阻塞开销可忽略
 *   - SDP 含换行/特殊字符，必须经 cJSON 规范转义后再进入 JSON 载荷
 *   - sid（会话ID）由上层生成并校验，本层透传
 */
#include "P2PSignaling.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* MQTT 参数：QoS 1（信令不容丢失）、10s 超时 */
static const int  kQos = 1;
static const long kTimeoutMs = 10000L;

P2PSignaling::P2PSignaling(const std::string& brokerUrl, const std::string& devId,
                           const std::string& role)
    : m_brokerUrl(brokerUrl), m_devId(devId), m_role(role) {
    m_topicPrefix = "p2p/" + devId;
}

P2PSignaling::~P2PSignaling() {
    disconnect();
}

bool P2PSignaling::connect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_connected) return true;

    /* clientId 携带角色与设备 ID，避免与业务 MQTT 连接冲突 */
    std::string clientId = "p2p_" + m_role + "_" + m_devId;

    int rc = MQTTClient_create(&m_client, m_brokerUrl.c_str(), clientId.c_str(),
                               MQTTCLIENT_PERSISTENCE_NONE, nullptr);
    if (rc != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "[P2PSignaling] MQTTClient_create failed rc=%d\n", rc);
        return false;
    }

    MQTTClient_setCallbacks(m_client, this, onConnectionLost, onMessageArrived, nullptr);

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.keepAliveInterval = 30;
    opts.cleansession = 1;           /* 信令会话无需持久化历史消息 */
    opts.connectTimeout = 10;

    rc = MQTTClient_connect(m_client, &opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "[P2PSignaling] connect %s failed rc=%d\n", m_brokerUrl.c_str(), rc);
        MQTTClient_destroy(&m_client);
        m_client = nullptr;
        return false;
    }

    /* 按角色订阅：pusher 收 offer + viewer 候选；viewer 收 answer + pusher 候选；bye 双方都收 */
    std::string t1, t2;
    if (m_role == "pusher") {
        t1 = m_topicPrefix + "/offer";
        t2 = m_topicPrefix + "/cand/viewer";
    } else {
        t1 = m_topicPrefix + "/answer";
        t2 = m_topicPrefix + "/cand/pusher";
    }
    std::string t3 = m_topicPrefix + "/bye";

    if (MQTTClient_subscribe(m_client, t1.c_str(), kQos) != MQTTCLIENT_SUCCESS ||
        MQTTClient_subscribe(m_client, t2.c_str(), kQos) != MQTTCLIENT_SUCCESS ||
        MQTTClient_subscribe(m_client, t3.c_str(), kQos) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "[P2PSignaling] subscribe failed\n");
        MQTTClient_disconnect(m_client, 1000);
        MQTTClient_destroy(&m_client);
        m_client = nullptr;
        return false;
    }

    m_connected = true;
    printf("[P2PSignaling] connected %s as %s (subscribed: %s, %s, %s)\n",
           m_brokerUrl.c_str(), clientId.c_str(), t1.c_str(), t2.c_str(), t3.c_str());
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

bool P2PSignaling::sendBye(const std::string& sid) {
    return publishJson(m_topicPrefix + "/bye", sid, nullptr, "");
}

bool P2PSignaling::publishJson(const std::string& topic, const std::string& sid,
                               const char* bodyKey, const std::string& bodyVal) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected || m_client == nullptr) {
        fprintf(stderr, "[P2PSignaling] publish skipped: not connected\n");
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
        fprintf(stderr, "[P2PSignaling] publish %s failed rc=%d\n", topic.c_str(), rc);
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
        /* 从 topic 提取消息类型：p2p/{devId}/offer|answer|bye|cand/{role} */
        std::string type;
        if (topic == self->m_topicPrefix + "/offer")        type = "offer";
        else if (topic == self->m_topicPrefix + "/answer")  type = "answer";
        else if (topic == self->m_topicPrefix + "/bye")     type = "bye";
        else if (topic.rfind(self->m_topicPrefix + "/cand/", 0) == 0) type = "candidate";
        if (type.empty()) break;

        std::string payload((const char*)message->payload, message->payloadlen);
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            fprintf(stderr, "[P2PSignaling] JSON parse failed: %.128s\n", payload.c_str());
            break;
        }

        cJSON* jsid = cJSON_GetObjectItem(root, "sid");
        std::string sid = (jsid && cJSON_IsString(jsid)) ? jsid->valuestring : "";
        std::string body;
        cJSON* jbody = cJSON_GetObjectItem(root, type == "candidate" ? "candidate" : "sdp");
        if (jbody && cJSON_IsString(jbody)) body = jbody->valuestring;
        cJSON_Delete(root);

        self->m_handler(type, sid, body);
    } while (0);

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;   /* 1=消息已处理（paho 约定） */
}

void P2PSignaling::onConnectionLost(void* ctx, char* cause) {
    P2PSignaling* self = (P2PSignaling*)ctx;
    fprintf(stderr, "[P2PSignaling] connection lost: %s\n", cause ? cause : "unknown");
    if (self) {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_connected = false;
    }
}
