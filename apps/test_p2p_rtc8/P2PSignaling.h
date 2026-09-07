/**
 * P2PSignaling.h - P2P 信令通道（MQTT 实现）
 *
 * 职责：
 *   - 仅负责 offer/answer/candidate/bye 等 JSON 信令消息的双向搬运
 *   - 不感知 WebRTC 连接细节（与 P2PStreamer/P2PViewer 通过回调解耦，
 *     将来可替换为 WebSocket/HTTP 信令而不改动连接层）
 *
 * Topic 约定（{devId} 为设备标识）：
 *   p2p/{devId}/offer        viewer → device   {"sid":"...","sdp":"..."}
 *   p2p/{devId}/answer       device → viewer   {"sid":"...","sdp":"..."}
 *   p2p/{devId}/cand/viewer  viewer → device   {"sid":"...","candidate":"..."}
 *   p2p/{devId}/cand/pusher  device → viewer   {"sid":"...","candidate":"..."}
 *   p2p/{devId}/bye          双向              {"sid":"..."}
 */
#ifndef P2P_SIGNALING_H
#define P2P_SIGNALING_H

#include <string>
#include <functional>
#include <mutex>

#include "MQTTClient.h"

class P2PSignaling {
public:
    /* 消息回调：type 为 offer/answer/candidate/bye，sid 为会话ID，body 为 sdp 或 candidate 文本 */
    using MessageHandler = std::function<void(const std::string& type,
                                              const std::string& sid,
                                              const std::string& body)>;

    /* role: "pusher"=设备端（收 offer/cand-viewer），"viewer"=观看端（收 answer/cand-pusher） */
    P2PSignaling(const std::string& brokerUrl, const std::string& devId,
                 const std::string& role);
    ~P2PSignaling();

    /* 连接 broker 并订阅本端角色所需 topic */
    bool connect();
    void disconnect();
    bool isConnected() const;

    /* 发送信令（内部按角色映射到对应 topic，JSON 封装由本类完成） */
    bool sendOffer(const std::string& sid, const std::string& sdp);
    bool sendAnswer(const std::string& sid, const std::string& sdp);
    bool sendCandidate(const std::string& sid, const std::string& candidate);
    bool sendBye(const std::string& sid);

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
    std::string m_topicPrefix;      /* p2p/{devId} */
    MessageHandler m_handler;
    mutable std::mutex m_mutex;     /* paho 同步客户端多线程 publish 保护 */
    bool m_connected = false;
};

#endif /* P2P_SIGNALING_H */
