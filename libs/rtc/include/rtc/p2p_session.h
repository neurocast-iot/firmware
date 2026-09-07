/**
 * @file p2p_session.h
 * @brief P2P 会话（设备端，answer 方，Full-ICE，metaRTC 8.0）
 *
 * 源自 test_p2p_rtc8/P2PStreamer（已上板验证），帧管线抽入 FrameSender
 * 后本类只剩连接管理。角色设计（与 metaRTC 8.0 语义对齐）：
 *   - 设备 = answer 方（isControlled=true，DTLS passive）
 *   - viewer = offer 方（浏览器原生，DTLS active）
 *   - 双端均为 Full-ICE：gather 线程真实执行，STUN/TURN 原生生效
 *
 * 会话时序（单会话）：
 *   handleOffer(offer, answer) → 上层经信令回发 answer
 *   → onIceCandidate 回调产出本端候选（上层经信令外发）
 *   → handleRemoteCandidate 注入对端候选
 *   → isConnected 轮询就绪 → startSending → pushFrame(相机帧)
 *   → stopSession（幂等，可重复 handleOffer 开启新会话）
 */
#pragma once

#include "rtc/frame_sender.h"
#include "rtc/ice_config.h"

#include <atomic>
#include <functional>
#include <string>

#include <yangrtc/YangPeerConnection8.h>

namespace rtc {

/**
 * P2P 会话配置（视频参数取相机实际生效值，不硬编码）
 */
struct P2PSessionConfig {
    /* 视频参数（必须显式传入） */
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate = 0;            /* kbps */

    /* RTC 参数（与 WHIP 推流端口 17000 错开，支持共存） */
    int rtcLocalPort = 17100;

    /* 音频参数（仅参与 SDP 协商，当前不推音频帧） */
    int audioSample  = 48000;
    int audioChannel = 2;

    /* ICE 服务器（host 允许域名，handleOffer 内每次解析） */
    IceConfig ice;

    bool isValid() const {
        return width > 0 && height > 0 && fps > 0 && bitrate > 0;
    }
};

/**
 * P2PSession - 设备端 P2P 会话
 *
 * 继承 metaRTC 回调基类：ICE 事件（候选/连接状态）+ RTC 请求 + SSL 告警；
 * 设备为 Sendonly，不实现 YangCallbackReceive（构造传 NULL）
 */
class P2PSession : public YangCallbackIce,
                   public YangCallbackRtc,
                   public YangCallbackSslAlert {
public:
    /* 本端 ICE 候选外发回调（gather 线程触发，上层负责经信令通道发送） */
    using CandidateHandler = std::function<void(const std::string& candidate)>;
    /* 关键帧请求回调（PLI 等 RTC 请求触发，上层接到相机 requestKeyFrame） */
    using KeyFrameHandler = std::function<void()>;

    P2PSession() = default;
    ~P2PSession() override;

    P2PSession(const P2PSession&) = delete;
    P2PSession& operator=(const P2PSession&) = delete;

    void setCandidateHandler(CandidateHandler handler);
    void setKeyFrameHandler(KeyFrameHandler handler);

    /**
     * 处理远端 offer：重建 PeerConnection → setRemote(offer) → createAnswer
     * → setLocal(answer)（内部启动 UDP socket 与 ICE agent）
     *
     * ICE 域名解析在本函数内执行；解析失败降级为仅 Host 候选
     * （局域网直连仍可用，不因公网服务器不可达而整体失败）。
     *
     * @param cfg       会话配置（每会话重新传入，取相机当前生效参数）
     * @param offerSdp  远端 offer SDP
     * @param answerSdp 输出本端 answer SDP（已还原标准 CRLF，可直接经信令回发）
     */
    bool handleOffer(const P2PSessionConfig& cfg, const std::string& offerSdp,
                     std::string& answerSdp);

    /* 注入远端 Trickle 候选 */
    bool handleRemoteCandidate(const std::string& candidate);

    /* 非阻塞查询 ICE/DTLS/SRTP 就绪状态（供服务线程轻量轮询；
     * 非 const：底层 YangPeerConnection8::isConnected 未声明 const） */
    bool isConnected();

    /* 连接就绪后启动发送管线，开始消费 pushFrame 入队的帧 */
    bool startSending();

    /* 入队一帧（未 startSending 时丢弃），参数语义见 FrameSender::pushFrame */
    void pushFrame(const uint8_t* data, int len, uint64_t timestampMs, bool keyframe);

    /* 结束会话（幂等：任意状态下调用均安全，支持重复 handleOffer） */
    void stopSession();

    bool isSessionActive() const;

    /* 对端已发 DTLS close_notify（信令 bye 丢失时的兜底终止信号，
     * 由服务线程轮询后调用 stopSession，不在回调线程内自毁） */
    bool isPeerClosed() const;

    /* ===== YangCallbackIce ===== */
    void onIceStateChange(int32_t uid, YangIceCandidateState iceState) override;
    void onConnectionStateChange(int32_t uid, YangRtcConnectionState state) override;
    void onIceCandidate(int32_t uid, char* candidate) override;
    void onIceGatheringState(int32_t uid, YangIceGatheringState gatherState) override;

    /* ===== YangCallbackRtc ===== */
    void setMediaConfig(int32_t uid, YangAudioParam* audio, YangVideoParam* video) override;
    void sendRequest(int32_t uid, uint32_t ssrc, YangRequestType req) override;

    /* ===== YangCallbackSslAlert ===== */
    void sslCloseAlert(int32_t uid) override;

private:
    static const int ANSWER_SDP_SIZE = 16 * 1024;   /* createAnswer 输出缓冲区 */

    void destroyConnection();

    FrameSender m_sender;

    /* ===== MetaRTC 8.0 连接 ===== */
    YangPeerConnection8* m_conn = nullptr;

    /* ===== 状态 ===== */
    std::atomic<bool> m_sessionActive{false};
    std::atomic<bool> m_peer_closed{false};   /* sslCloseAlert 回调置位 */
    P2PSessionConfig m_config;
    CandidateHandler m_candHandler;
    KeyFrameHandler m_keyFrameHandler;
};

} // namespace rtc
