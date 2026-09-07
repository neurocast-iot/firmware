/**
 * @file LiveStreamService.h
 * @brief 实时视频服务：P2P（单人）+ SFU（多人/兜底）三态状态机
 *
 * 设备只做执行者，不做自动降级决策：
 *   - P2P 模式：观看端发 offer → 设备走 P2P，失败/超时发 bye 通知前端
 *   - SFU 模式：前端 RPC startLiveStream → 设备起 WHIP 推流到 SRS
 *   - P2P 多人：停当前 P2P → 发 sfu 通知观看者 → 前端自行 RPC 起 SFU
 *
 * 状态机（全部转换在工作线程内完成）：
 *   Idle ──收到 offer (P2P模式)──→ P2P（单人会话，全候选 ICE 自动选路）
 *   P2P  ──bye / DTLS close──────→ Idle
 *   P2P  ──连接超时(15s)─────────→ Idle（发 bye 通知前端）
 *   P2P  ──第 2 个 offer─────────→ Idle（停 P2P，发 sfu 通知，前端决定下一步）
 *   SFU  ──再来 offer────────────→ 拒绝（发 bye "sfu_active"，前端自行决定下一步）
 *   SFU  ──全部心跳消失(30s)─────→ 停 WHIP 推流 → Idle
 *
 * 线程模型（复刻 test_p2p_pusher 已验证模型）：
 *   - paho 回调线程：仅将信令消息入队（回调内 publish 会与 PUBACK 死锁）
 *   - 工作线程：消费队列 + 500ms 周期 tick（连接轮询/超时/心跳清理/信令重连）
 *   - 相机采集线程：帧回调直通 pushFrame（内部按 running 标志自然丢弃）
 *
 * 帧路由：进入 P2P/SFU 时 subscribe(Main)（订阅驱动主通道自动启用），
 * 回 Idle 时退订；视频参数每次建会话时从 device()->config().mainChannel()
 * 取实际生效值，不硬编码。
 *
 * 相机重启编排：pauseForRestart = 结束所有会话（发 bye / 停推流，
 * 观看端自行重连）；resumeAfterRestart 无动作。
 */
#pragma once

#include "config/ConfigStore.h"

#include "camera/camera_device.h"
#include "rtc/p2p_session.h"
#include "rtc/p2p_signaling.h"
#include "rtc/whip_pusher.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mediad {

class CameraService;

class LiveStreamService {
public:
    /** 推流模式（设备推出去的方式） */
    enum class PushMode { P2P, P2P_Host, SFU };

    /** 状态快照（MEDIA_GET_STATUS 用） */
    struct Status {
        std::string mode;         ///< "idle" / "p2p" / "sfu"
        int         viewers    = 0;
        bool        manualPush = false;   ///< mediactl 手动推流保持中
    };

    explicit LiveStreamService(CameraService& camera) : m_camera(camera) {}
    ~LiveStreamService();

    LiveStreamService(const LiveStreamService&) = delete;
    LiveStreamService& operator=(const LiveStreamService&) = delete;

    /**
     * 启动服务：拉起工作线程并连接信令 broker
     * （连接失败不阻断启动：4G 场景开机网络未必就绪，工作线程 5s 退避重连）
     * cfg.enabled=false 或 devId 为空（尚未被 IoT 进程下发）时空转返回，不连 broker。
     *
     * @param devId 设备标识（取 MediadConfig::device.id），决定信令 topic 与 SRS 流名
     */
    void start(const std::string& devId, const LiveCfg& cfg);

    /**
     * 应用新配置：devId/enabled/mqttSignaling.url 变化 → 整体重启服务；
     * 其余字段（超时/SRS 地址/ICE 凭证/MQTT 认证）热更，下次建会话生效。
     * devId 从空变为实值（IoT 进程首次下发）也走重启分支，即时接入信令。
     */
    void apply(const std::string& devId, const LiveCfg& cfg);

    /** 停止服务：结束所有会话（发 bye/停推流）→ 断开信令 → 收线程（幂等） */
    void stop();

    /**
     * 相机重启前置：结束所有会话并退订主通道
     * （返回即保证不再有任何线程向 RTC 管线喂帧）
     */
    void pauseForRestart();

    /** 相机重启完成：无动作（观看端收 bye 后自行重连，幂等） */
    void resumeAfterRestart();

    /** 手动推流错误码（startManualPush / stopManualPush 返回） */
    enum class ManualPushError {
        Ok,              ///< 成功
        Disabled,        ///< 直播服务未启用
        Busy,            ///< 资源忙（相机重启中）
        TokenMismatch,   ///< token 不匹配，无权停止
        Internal,        ///< 内部失败（如 SRS 连不上）
    };

    /** 错误码转可读文本（给应答的 message 字段用） */
    static const char* toManualPushErrorMsg(ManualPushError err) {
        switch (err) {
            case ManualPushError::Ok:            return "ok";
            case ManualPushError::Disabled:      return "live service disabled";
            case ManualPushError::Busy:          return "camera restarting";
            case ManualPushError::TokenMismatch: return "token mismatch";
            case ManualPushError::Internal:      return "whip push failed";
            default:                             return "unknown error";
        }
    }

    /**
     * 手动起 SRS 推流（MEDIA_STREAM_START，iot_agent RPC 调用）
     * 置 manualPush 保持标志：无观看者也不会被 30s 空闲回收。
     * 此接口固定走 SFU 模式（跳过 P2P，直接起 WHIP 推流）。
     * P2P 模式由观看端通过 MQTT 信令发 offer 触发，不走此接口。
     * @param accessToken 平台下发的认证令牌（拼进 WHIP URL）
     */
    ManualPushError startManualPush(const std::string& accessToken);

    /**
     * 手动停 SRS 推流（MEDIA_STREAM_STOP）：清除保持标志；
     * 若无 sfu 观看者则立即停推回 Idle，有观看者则维持推流。
     * @param accessToken 调用方传入的 token，必须与当前推流 token 一致才允许停止
     */
    ManualPushError stopManualPush(const std::string& accessToken);

    Status status() const;

private:
    enum class Mode { Idle, P2P, SFU };

    /** 信令消息（paho 回调线程入队，工作线程消费） */
    struct SigMsg {
        std::string type;   ///< offer/candidate/bye/alive
        std::string sid;
        std::string body;
        std::string mode;   ///< offer 消息的推流模式（"p2p" / "p2p_host"，空=默认 p2p）
    };

    void workerLoop();
    /* 以下状态机动作均在工作线程内持 m_mutex 调用 */
    void processMessage(const SigMsg& msg);
    void tickLocked(int64_t nowMs);
    /* offer 处理：P2P / P2P_Host 模式 */
    void handleOfferP2P(const std::string& sid, const std::string& offerSdp);
    bool enterP2P(const std::string& sid, const std::string& offerSdp);
    bool ensureWhipPushing();                       ///< 起 WHIP 推流（幂等）
    void teardownAllLocked(bool notifyPeers);
    void subscribeMain();
    void unsubscribeMain();
    void requestKeyFrame();

    static const char* stateName(Mode m);
    static const char* pushModeName(PushMode p);
    static int64_t nowMs();

    CameraService& m_camera;

    /* ===== RTC 组件（对象常驻，会话级生命周期由各自 start/stop 管理） ===== */
    std::unique_ptr<rtc::P2PSignaling> m_signaling;
    rtc::P2PSession m_session;
    rtc::WhipPusher m_pusher;

    /* ===== 状态机（m_mutex 保护） ===== */
    mutable std::mutex m_mutex;
    std::string m_devId;                  ///< 设备标识（应用级配置传入，决定 topic 与流名）
    LiveCfg   m_cfg;
    Mode      m_mode = Mode::Idle;
    PushMode  m_pushMode = PushMode::P2P;  ///< 推流模式（默认 P2P）
    bool      m_p2pConnected = false;     ///< P2P 已就绪并开始发送
    int64_t   m_p2pStartMs   = 0;         ///< P2P 协商起点（超时判定基准）
    bool      m_manualPush   = false;     ///< 手动推流保持（空闲不回收）
    std::string m_accessToken;            ///< 平台下发的认证令牌（拼进 WHIP URL）
    std::string m_pushToken;              ///< 当前推流实际使用的 token（用于检测 token 变化）
    std::map<std::string, int64_t> m_sfuViewers;     ///< sid → 最后心跳时刻(ms)
    camera::SubscriptionId m_subId = camera::kInvalidSubscriptionId;
    std::atomic<bool> m_paused{false};    ///< 相机重启窗口内拒绝新会话

    /* P2P 会话 sid：候选外发回调在 metaRTC gather 线程读取，
     * 独立小锁避免与 m_mutex 内的 stopSession（join gather 线程）互锁 */
    std::mutex  m_sidMutex;
    std::string m_p2pSid;

    /* ===== 信令消息队列 ===== */
    std::mutex              m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<SigMsg>      m_queue;

    std::thread       m_thread;
    std::atomic<bool> m_stopFlag{false};
};

} // namespace mediad
