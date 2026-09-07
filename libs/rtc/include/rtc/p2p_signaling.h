/**
 * @file p2p_signaling.h
 * @brief P2P 信令通道（MQTT 实现，自 test_p2p_rtc8/P2PSignaling 迁入）
 *
 * 职责：
 *   - 仅负责 offer/answer/candidate/bye/relay/alive 等 JSON 信令消息的双向搬运
 *   - 不感知 WebRTC 连接细节（与 P2PSession 通过回调解耦，
 *     将来可替换为 WebSocket/HTTP 信令而不改动连接层）
 *
 * Topic 约定（{devId} 为设备标识）：
 *   p2p/{devId}/offer        viewer → device   {"sid":"...","sdp":"..."}
 *   p2p/{devId}/answer       device → viewer   {"sid":"...","sdp":"..."}
 *   p2p/{devId}/cand/viewer  viewer → device   {"sid":"...","candidate":"..."}
 *   p2p/{devId}/cand/pusher  device → viewer   {"sid":"...","candidate":"..."}
 *   p2p/{devId}/bye          双向              {"sid":"..."}
 *   p2p/{devId}/relay        device → viewer   {"sid":"...","url":"..."}  改走 SRS WHEP 拉流
 *   p2p/{devId}/alive        viewer → device   {"sid":"..."}  10s 心跳（SRS 模式观看人数追踪）
 *
 * 线程约束：onMessageArrived 运行在 paho 回调线程，回调内同步 publish
 * 会与 PUBACK 处理死锁——上层 handler 只做入队，由服务工作线程消费。
 */
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "MQTTClient.h"

namespace rtc {

class P2PSignaling {
public:
    /* 消息回调：type 为 offer/answer/candidate/bye/relay/alive，
     * sid 为会话ID，body 为 sdp/candidate/url 文本（bye/alive 为空） */
    using MessageHandler = std::function<void(const std::string& type,
                                              const std::string& sid,
                                              const std::string& body)>;

    /* role: "pusher"=设备端（收 offer/cand-viewer/alive），
     *       "viewer"=观看端（收 answer/cand-pusher/relay）
     * username/password: MQTT 认证凭据（可为空，表示无认证） */
    P2PSignaling(const std::string& brokerUrl, const std::string& devId,
                 const std::string& role,
                 const std::string& username = "",
                 const std::string& password = "");
    ~P2PSignaling();

    P2PSignaling(const P2PSignaling&) = delete;
    P2PSignaling& operator=(const P2PSignaling&) = delete;

    /* 连接 broker 并订阅本端角色所需 topic */
    bool connect();
    void disconnect();
    bool isConnected() const;

    /* 发送信令（内部按角色映射到对应 topic，JSON 封装由本类完成） */
    bool sendOffer(const std::string& sid, const std::string& sdp);
    bool sendAnswer(const std::string& sid, const std::string& sdp);
    bool sendCandidate(const std::string& sid, const std::string& candidate);
    bool sendBye(const std::string& sid, const std::string& reason = "");
    /* 设备端：通知观看者改走 SRS WHEP 拉流 */
    bool sendRelay(const std::string& sid, const std::string& whepUrl);
    /* 观看端：10s 心跳（C++ 端预留，viewer.html 为主要使用方） */
    bool sendAlive(const std::string& sid);

    void setMessageHandler(MessageHandler handler);

private:
    bool publishJson(const std::string& topic, const std::string& sid,
                     const char* bodyKey, const std::string& bodyVal);

    /* paho 静态回调包装 */
    static int  onMessageArrived(void* ctx, char* topicName, int topicLen,
                                 MQTTClient_message* message);
    static void onConnectionLost(void* ctx, char* cause);

    MQTTClient m_client = nullptr;
    std::string m_brokerUrl;
    std::string m_devId;
    std::string m_role;
    std::string m_username;           /* MQTT 认证用户名（空=无认证） */
    std::string m_password;           /* MQTT 认证密码 */
    std::string m_topicPrefix;      /* p2p/{devId} */
    MessageHandler m_handler;
    mutable std::mutex m_mutex;     /* paho 同步客户端多线程 publish 保护 */
    bool m_connected = false;
};

} // namespace rtc
