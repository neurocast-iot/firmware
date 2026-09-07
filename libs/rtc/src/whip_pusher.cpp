/**
 * @file whip_pusher.cpp
 * @brief WHIP 推流器实现（自 WebRTCStreamer 迁入，帧管线换用 FrameSender）
 */
#include "rtc/whip_pusher.h"

#include <nc/common/log_utils.h>

#include <yangrtc/YangWhip.h>
#include <yangrtc/YangPeerInfo.h>
#include <yangutil/yangavinfo.h>
#include <yangutil/sys/YangTime.h>

#include <cstring>
#include <unistd.h>

namespace rtc {

WhipPusher::~WhipPusher() {
    stopPush();
}

bool WhipPusher::startPush(const WhipConfig& cfg, const std::string& whipUrl) {
    if (!cfg.isValid() || whipUrl.empty()) {
        NC_LOGE("[WhipPusher] config invalid: {}x{}@{} {}kbps url={}",
                cfg.width, cfg.height, cfg.fps, cfg.bitrate, whipUrl.c_str());
        return false;
    }
    if (m_pushing.load()) {
        NC_LOGW("[WhipPusher] already pushing");
        return false;
    }

    NC_LOGI("[WhipPusher] startPush: {}x{}@{} {}kbps port={} url={}",
            cfg.width, cfg.height, cfg.fps, cfg.bitrate,
            cfg.rtcLocalPort, whipUrl.c_str());

    /* ---- 阶段1: 初始化 YangAVInfo（WHIP 路径特有 mediaServer 标志） ---- */
    YangAVInfo avinfo;
    memset(&avinfo, 0, sizeof(YangAVInfo));
    avinfo.sys.mediaServer = Yang_Server_Whip_Whep;
    avinfo.audio.sample = cfg.audioSample;
    avinfo.audio.channel = cfg.audioChannel;
    avinfo.audio.audioEncoderType = Yang_AED_OPUS;
    avinfo.audio.enableAudioFec = yangfalse;
    avinfo.video.width = cfg.width;
    avinfo.video.height = cfg.height;
    avinfo.video.outWidth = cfg.width;
    avinfo.video.outHeight = cfg.height;
    avinfo.video.rate = cfg.bitrate;    /* kbps，供 SDP/带宽估计，实际码率由 VENC 控制 */
    avinfo.video.frame = cfg.fps;
    avinfo.video.videoCacheNum = 10;
    avinfo.video.evideoCacheNum = 10;
    avinfo.video.videoPlayCacheNum = 10;
    avinfo.video.videoEncoderType = Yang_VED_H264;
    avinfo.rtc.rtcLocalPort = cfg.rtcLocalPort;
    avinfo.enc.enc_threads = 4;

    /* ---- 阶段2: 创建 PeerConnection8（offer 方，无回调注入） ---- */
    YangPeerInfo peerInfo;
    yang_avinfo_initPeerInfo(&peerInfo, &avinfo);
    peerInfo.direction = YangSendonly;

    m_conn = new YangPeerConnection8(&peerInfo, NULL, NULL, NULL, NULL);
    m_conn->addAudioTrack(Yang_AED_OPUS);
    m_conn->addVideoTrack(Yang_VED_H264);
    m_conn->addTransceiver(YangMediaAudio, peerInfo.direction);
    m_conn->addTransceiver(YangMediaVideo, peerInfo.direction);

    /* ---- 阶段3: WHIP 连接 SRS（HTTP SDP 交换） ---- */
    uint64_t whip_start_ms = yang_get_system_time() / 1000;
    int ret = yang_whip_connectWhipWhepServer(&m_conn->m_peer, (char*)whipUrl.c_str());
    uint64_t whip_elapsed = yang_get_system_time() / 1000 - whip_start_ms;

    NC_LOGI("[WhipPusher] WHIP connect: ret={}, elapsed={}ms",
            ret, whip_elapsed);
    if (ret) {
        NC_LOGE("[WhipPusher] WHIP connect failed ret={}", ret);
        delete m_conn;
        m_conn = nullptr;
        return false;
    }

    /* ---- 阶段4: 等待 WebRTC 底层就绪（ICE/DTLS/SRTP）----
     * 事件驱动轮询 isConnected()，就绪即放行；超时 10s 判定失败并清理退出 */
    int wait_ms = 0;
    while (!m_conn->isConnected() && wait_ms < 10000) {
        usleep(100 * 1000);
        wait_ms += 100;
    }
    if (!m_conn->isConnected()) {
        NC_LOGE("[WhipPusher] not connected after {}ms (ICE/DTLS/SRTP timeout)", wait_ms);
        delete m_conn;
        m_conn = nullptr;
        return false;
    }
    NC_LOGI("[WhipPusher] connected (ICE/DTLS/SRTP ready, waited {}ms)", wait_ms);

    /* ---- 阶段5: 启动发送管线（Pacer + 发送线程） ---- */
    if (!m_sender.start(m_conn, cfg.audioSample, cfg.audioChannel)) {
        NC_LOGE("[WhipPusher] sender start failed");
        delete m_conn;
        m_conn = nullptr;
        return false;
    }

    m_pushing.store(true);
    NC_LOGI("[WhipPusher] push started");
    return true;
}

void WhipPusher::stopPush() {
    if (!m_pushing.load() && m_conn == nullptr) return;

    NC_LOGI("[WhipPusher] stopPush...");
    m_pushing.store(false);

    /* 先停发送管线（join 线程后 conn 不再被触碰），再销毁连接 */
    m_sender.stop();
    if (m_conn) {
        delete m_conn;
        m_conn = nullptr;
    }
    NC_LOGI("[WhipPusher] push stopped");
}

bool WhipPusher::isPushing() const {
    return m_pushing.load();
}

void WhipPusher::pushFrame(const uint8_t* data, int len,
                           uint64_t timestampMs, bool keyframe) {
    if (!m_pushing.load()) return;
    m_sender.pushFrame(data, len, timestampMs, keyframe);
}

} // namespace rtc
