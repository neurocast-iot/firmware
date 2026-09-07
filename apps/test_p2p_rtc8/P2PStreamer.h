/**
 * P2PStreamer.h - P2P 推流器（设备端，answer 方，metaRTC 8.0）
 *
 * 架构定位：
 *   - 与 WebRTCStreamer（WHIP 推流）平级并列，零侵入现有链路
 *   - 帧队列/发送线程/Pacer 喂帧逻辑与 WebRTCStreamer 保持一致（已验证格式）
 *   - 信令不感知：候选外发通过 CandidateHandler 注入，由上层对接 P2PSignaling
 *
 * 角色设计（与 metaRTC 8.0 语义对齐）：
 *   - 设备 = answer 方（createAnswer 内部置 isControlled=true，DTLS passive）
 *   - viewer = offer 方（复刻 WHIP/WHEP 客户端已验证路径，DTLS active）
 *   - 双端均为 Full-ICE：gather 线程真实执行，STUN/TURN 原生生效
 *     （对 SRS 的 ICE-Lite 跳过问题在 P2P 场景不存在）
 *
 * 会话时序（单会话）：
 *   handleOffer(offer, answer) → 上层经信令回发 answer
 *   → onIceCandidate 回调产出本端候选（上层经信令外发）
 *   → handleRemoteCandidate 注入对端候选
 *   → waitConnected → startSending → pushVideoFrame(帧回调)
 *   → stopSession（幂等，可重复 handleOffer 开启新会话）
 */
#ifndef P2P_STREAMER_H
#define P2P_STREAMER_H

#include <string>
#include <atomic>
#include <functional>
#include <pthread.h>

class YangPeerConnection8;
class YangRtcPacer;
#include <yangrtc/YangPeerConnection8.h>

/**
 * P2PConfig - P2P 推流配置（字段语义与 WebRTCConfig 对齐）
 */
struct P2PConfig {
    /* 视频参数（必须显式传入） */
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate = 0;            /* kbps */

    /* RTC 参数 */
    int rtcLocalPort = 17100;   /* 与 WHIP demo(17000) 错开，支持共存调试 */

    /* 音频参数（仅参与 SDP 协商） */
    int audioSample  = 48000;
    int audioChannel = 2;

    /* ICE 候选策略: 0=Host 1=Stun 2=Turn */
    int iceCandidateType = 0;
    std::string iceServerIP = "";
    int iceServerPort = 3478;
    std::string iceUserName = "";
    std::string icePassword = "";

    bool isValid() const {
        return width > 0 && height > 0 && fps > 0 && bitrate > 0;
    }
};

/**
 * P2PStreamer - 设备端 P2P 推流器
 *
 * 继承 metaRTC 回调基类：ICE 事件（候选/连接状态）+ RTC 请求 + SSL 告警；
 * 设备为 Sendonly，不实现 YangCallbackReceive（构造传 NULL）
 */
class P2PStreamer : public YangCallbackIce,
                    public YangCallbackRtc,
                    public YangCallbackSslAlert {
public:
    /* 本端 ICE 候选外发回调（gather 线程触发，上层负责经信令通道发送） */
    using CandidateHandler = std::function<void(const std::string& candidate)>;

    P2PStreamer();
    ~P2PStreamer();

    bool init(const P2PConfig& config);
    void setCandidateHandler(CandidateHandler handler);

    /**
     * 处理远端 offer：重建 PeerConnection → setRemote(offer) → createAnswer
     * → setLocal(answer)（内部启动 UDP socket 与 ICE agent）
     * @param offerSdp  远端 offer SDP
     * @param answerSdp 输出本端 answer SDP（经信令回发）
     */
    bool handleOffer(const std::string& offerSdp, std::string& answerSdp);

    /* 注入远端 Trickle 候选 */
    bool handleRemoteCandidate(const std::string& candidate);

    /* 等待 ICE/DTLS/SRTP 就绪（事件驱动轮询，超时返回 false） */
    bool waitConnected(int timeoutMs);

    /* 非阻塞查询连接就绪状态（供主循环轻量轮询，不打印超时日志；
     * 非 const：底层 YangPeerConnection8::isConnected 未声明 const） */
    bool isConnected();

    /* 启动发送线程，开始消费帧队列 */
    bool startSending();

    /* 结束会话（幂等：任意状态下调用均安全，支持重复 handleOffer） */
    void stopSession();

    bool isSessionActive() const;

    /* 对端已发 DTLS close_notify（信令 bye 丢失时的兜底终止信号，
     * 由主循环轮询后调用 stopSession，不在回调线程内自毁） */
    bool isPeerClosed() const;

    /* 视频帧回调（C 风格，与 WebRTCStreamer::frameCallback 签名一致） */
    static void frameCallback(unsigned char* data, int len,
                              unsigned long long timestamp, int is_keyframe);

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
    /* ===== 帧队列常量（与 WebRTCStreamer 一致） ===== */
    static const int FRAME_QUEUE_SIZE = 32;
    static const int MAX_FRAME_SIZE = 256 * 1024;
    static const int ANSWER_SDP_SIZE = 16 * 1024;   /* createAnswer 输出缓冲区 */

    struct FrameItem {
        unsigned char data[MAX_FRAME_SIZE];
        int len;
        int64_t pts;            /* 微秒（metaRTC 内部转 90kHz） */
        int is_keyframe;
    };

    static void* senderThreadFunc(void* arg);
    void destroyConnection();

    /* ===== 帧队列(生产者-消费者) — 堆分配避免栈溢出 ===== */
    FrameItem* m_frame_queue = nullptr;
    int m_q_write = 0;
    int m_q_read = 0;
    int m_q_count = 0;
    pthread_mutex_t m_q_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t m_q_cond = PTHREAD_COND_INITIALIZER;
    pthread_t m_sender_thread;
    std::atomic<bool> m_sender_running{false};
    int m_q_dropped = 0;

    /* ===== 首帧时间基准 ===== */
    unsigned long long m_first_hw_ts = 0;

    /* ===== MetaRTC 8.0 组件 ===== */
    YangPeerConnection8* m_conn = nullptr;
    YangRtcPacer* m_pacer = nullptr;

    /* ===== 状态 ===== */
    bool m_initialized = false;
    std::atomic<bool> m_sessionActive{false};
    std::atomic<bool> m_peer_closed{false};   /* sslCloseAlert 回调置位 */
    P2PConfig m_config;
    CandidateHandler m_candHandler;

    /* ===== 单例指针（供静态 C 回调访问实例） ===== */
    static P2PStreamer* s_instance;
};

#endif /* P2P_STREAMER_H */
