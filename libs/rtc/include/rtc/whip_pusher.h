/**
 * @file whip_pusher.h
 * @brief WHIP 推流器（推 SRS，metaRTC 8.0）
 *
 * 源自 test_easy_rtc8/WebRTCStreamer（已上板验证），帧管线抽入 FrameSender
 * 后本类只剩连接管理。与 P2PSession 的关键差异：
 *   1. 信令：WHIP HTTP（yang_whip_connectWhipWhepServer）→ 无外部信令层
 *   2. 角色：offer 方（mediaServer=Yang_Server_Whip_Whep，对端 SRS 为 ICE-Lite）
 *   3. 回调：无（构造回调参数全 NULL）
 *
 * 用途：SRS 兜底与多人分发链路（P2P 协商超时 / 第 2 观看者到来时启用），
 * 设备上行始终只有一路 WHIP 流，观看端经 WHEP 从 SRS 拉流。
 */
#pragma once

#include "rtc/frame_sender.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <yangrtc/YangPeerConnection8.h>

namespace rtc {

/**
 * WHIP 推流配置（视频参数取相机实际生效值，不硬编码）
 */
struct WhipConfig {
    /* 视频参数（必须显式传入） */
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate = 0;            /* kbps */

    /* RTC 参数（与 P2P 会话端口 17100 错开，支持共存） */
    int rtcLocalPort = 17000;

    /* 音频参数（仅参与 SDP 协商，当前不推音频帧） */
    int audioSample  = 48000;
    int audioChannel = 2;

    bool isValid() const {
        return width > 0 && height > 0 && fps > 0 && bitrate > 0;
    }
};

class WhipPusher {
public:
    WhipPusher() = default;
    ~WhipPusher();

    WhipPusher(const WhipPusher&) = delete;
    WhipPusher& operator=(const WhipPusher&) = delete;

    /**
     * 开始 WHIP 推流（阻塞：HTTP SDP 交换 + 等待 ICE/DTLS/SRTP 就绪，
     * 就绪即返回，超时 10s 判定失败并清理）
     *
     * 注意：对 SRS（ICE-Lite 且应答携带候选）metaRTC 跳过 gather 线程
     * （YangIce.c），行为为 Host 直连 SRS 公网地址，无需 STUN/TURN 配置。
     *
     * @param cfg     推流配置（每次启动重新传入，取相机当前生效参数）
     * @param whipUrl WHIP 服务器地址（http://host:port/rtc/v1/whip/?app=live&stream=xxx）
     */
    bool startPush(const WhipConfig& cfg, const std::string& whipUrl);

    /** 停止推流并释放连接（幂等） */
    void stopPush();

    bool isPushing() const;

    /* 入队一帧（未推流时丢弃），参数语义见 FrameSender::pushFrame */
    void pushFrame(const uint8_t* data, int len, uint64_t timestampMs, bool keyframe);

private:
    FrameSender m_sender;

    /* ===== MetaRTC 8.0 连接 ===== */
    YangPeerConnection8* m_conn = nullptr;

    /* ===== 状态 ===== */
    std::atomic<bool> m_pushing{false};
};

} // namespace rtc
