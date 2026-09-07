/**
 * @file p2p_session.cpp
 * @brief P2P 会话实现（自 P2PStreamer 迁入，帧管线换用 FrameSender）
 *
 * 与 WhipPusher 的关键差异：
 *   1. 信令：WHIP HTTP → offer/answer/candidate 由外部信令层搬运
 *   2. 角色：offer 方 → answer 方（isControlled=true，DTLS passive）
 *   3. ICE：YangIceModeFull + enableSdpCandidate，gather 线程真实执行，
 *      STUN/TURN 按 iceCandidateType 原生生效（全候选模式=Turn）
 *   4. 回调：构造时注入 YangCallbackIce（WHIP 路径传 NULL），
 *      onIceCandidate 产出 Trickle 候选交由上层信令外发
 */
#include "rtc/p2p_session.h"

#include <nc/common/log_utils.h>

#include <yangrtc/YangPeerInfo.h>
#include <yangutil/yangavinfo.h>

#include <cstdlib>
#include <cstring>

namespace rtc {

P2PSession::~P2PSession() {
    stopSession();
}

void P2PSession::setCandidateHandler(CandidateHandler handler) {
    m_candHandler = handler;
}

void P2PSession::setKeyFrameHandler(KeyFrameHandler handler) {
    m_keyFrameHandler = handler;
}

bool P2PSession::handleOffer(const P2PSessionConfig& cfg, const std::string& offerSdp,
                             std::string& answerSdp) {
    if (!cfg.isValid()) {
        NC_LOGE("[P2PSession] config invalid: {}x{}@{} {}kbps",
                cfg.width, cfg.height, cfg.fps, cfg.bitrate);
        return false;
    }

    /* ---- 阶段0: 幂等保障——清理上一会话残留（连接/线程/队列） ---- */
    stopSession();
    m_peer_closed.store(false);   /* 新会话复位对端关闭标志 */
    m_config = cfg;

    /* ---- 阶段0.5: ICE 域名解析（每次建会话执行，4G 开机网络未必就绪；
     * 解析失败降级为仅 Host 候选，局域网直连不受公网服务器影响） ---- */
    int iceType = cfg.ice.candidateType();
    std::string iceIp;
    if (iceType > 0) {
        if (resolveHostIPv4(cfg.ice.host, iceIp)) {
            NC_LOGI("[P2PSession] ice server {} -> {}:{} (type={})",
                    cfg.ice.host.c_str(), iceIp.c_str(), cfg.ice.port, iceType);
        } else {
            NC_LOGW("[P2PSession] ice server resolve failed: {}, degrade to Host-only",
                    cfg.ice.host.c_str());
            iceType = 0;
            iceIp.clear();
        }
    }

    /* ---- 阶段1: 构建 YangAVInfo → YangPeerInfo（参数装配与测试工程一致） ---- */
    YangAVInfo avinfo;
    memset(&avinfo, 0, sizeof(YangAVInfo));
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
    avinfo.rtc.iceCandidateType = iceType;
    /* ICE 字段使用 snprintf 防溢出（目标缓冲区为定长数组） */
    snprintf(avinfo.rtc.iceServerIP, sizeof(avinfo.rtc.iceServerIP), "{}", iceIp.c_str());
    avinfo.rtc.iceServerPort = cfg.ice.port;
    snprintf(avinfo.rtc.iceUserName, sizeof(avinfo.rtc.iceUserName), "{}", cfg.ice.username.c_str());
    snprintf(avinfo.rtc.icePassword, sizeof(avinfo.rtc.icePassword), "{}", cfg.ice.password.c_str());
    avinfo.enc.enc_threads = 4;

    YangPeerInfo peerInfo;
    yang_avinfo_initPeerInfo(&peerInfo, &avinfo);
    peerInfo.direction = YangSendonly;
    /*
     * P2P 关键配置（区别于 WHIP 路径在 YangWhip.c 中强制的 Lite 模式）：
     *   - iceMode=Full: 对端（viewer）为 Full-ICE，gather 线程真实执行
     *   - enableSdpCandidate: answer SDP 内联 Host 候选（srflx/relay 走 Trickle）
     *   - isControlled=true 必须在 setRemoteDescription 之前设置：
     *     该函数按此标志决定 DTLS 角色（true=passive）并跳过 initPlay
     *     （createAnswer 内部也会设置，但时序在 setRemote 之后，不能依赖）
     */
    peerInfo.iceMode = YangIceModeFull;
    peerInfo.rtc.enableSdpCandidate = yangtrue;
    peerInfo.rtc.isControlled = yangtrue;

    /* TURN 模式：Allocate 等待窗口默认仅 1000ms（YangAvtype.c initTurn
     * 折算 50次×20ms 轮询），在 CGNAT 链路上余量不足；放宽到 5s
     * （成功时循环提前退出不增加延迟） */
    if (iceType == 2) {
        peerInfo.rtc.maxTurnWaitTime = 5000;
    }

    /* ---- 阶段2: 创建 PeerConnection（注入 ICE/RTC/SSL 回调，Sendonly 无接收回调） ---- */
    m_conn = new YangPeerConnection8(&peerInfo, NULL, this, this, this);
    m_conn->addAudioTrack(Yang_AED_OPUS);
    m_conn->addVideoTrack(Yang_VED_H264);
    m_conn->addTransceiver(YangMediaAudio, YangSendonly);
    m_conn->addTransceiver(YangMediaVideo, YangSendonly);

    /* ---- 阶段3: SDP 协商（answer 方标准时序） ---- */
    int ret = m_conn->setRemoteDescription((char*)offerSdp.c_str());
    if (ret != 0) {
        NC_LOGE("[P2PSession] setRemoteDescription failed ret={}", ret);
        destroyConnection();
        return false;
    }

    char* answerBuf = (char*)calloc(1, ANSWER_SDP_SIZE);
    ret = m_conn->createAnswer(answerBuf);
    if (ret != 0 || answerBuf[0] == '\0') {
        NC_LOGE("[P2PSession] createAnswer failed ret={}", ret);
        free(answerBuf);
        destroyConnection();
        return false;
    }
    answerSdp = answerBuf;

    /* metaRTC 在 enableSdpCandidate 开启时会把 answer 的 CRLF 预转义为字面文本 "\r\n"
     * （YangSdp.c yang_sdp_genLocalSdp2），该格式仅适配其手工拼 JSON 的 demo 信令；
     * 本工程经 cJSON 规范转义，若不还原会双重转义，浏览器端 setRemoteDescription
     * 会因 SDP 整体被视为单行而报 "Expect line: v="。
     * 注意：setLocalDescription 仍使用原始转义缓冲 answerBuf（实测该路径正常），
     * 仅外发副本还原为标准 CRLF。 */
    size_t escPos = 0;
    while ((escPos = answerSdp.find("\\r\\n", escPos)) != std::string::npos) {
        answerSdp.replace(escPos, 4, "\r\n");
        escPos += 2;
    }

    /* setLocalDescription 内部创建 UDP socket 并启动 ICE agent（gather 线程） */
    ret = m_conn->setLocalDescription(answerBuf);
    free(answerBuf);
    if (ret != 0) {
        NC_LOGE("[P2PSession] setLocalDescription failed ret={}", ret);
        destroyConnection();
        return false;
    }

    m_sessionActive.store(true);
    NC_LOGI("[P2PSession] session negotiating (answer generated, {} bytes)",
            answerSdp.size());
    return true;
}

bool P2PSession::handleRemoteCandidate(const std::string& candidate) {
    if (m_conn == nullptr) {
        NC_LOGW("[P2PSession] candidate dropped (no connection)");
        return false;
    }
    /* metaRTC 的 addIceCandidate 要求 JSON 格式：{"candidate": "..."}
     * 前端发的是裸 SDP 字符串，这里包一层 JSON */
    std::string json = "{\"candidate\":\"" + candidate + "\"}";
    int ret = m_conn->addIceCandidate((char*)json.c_str());
    NC_LOGD("[P2PSession] addIceCandidate ret={}: %.80s", ret, candidate.c_str());
    return ret == 0;
}

bool P2PSession::isConnected() {
    return m_conn != nullptr && m_conn->isConnected();
}

bool P2PSession::startSending() {
    if (m_conn == nullptr) return false;
    return m_sender.start(m_conn, m_config.audioSample, m_config.audioChannel);
}

void P2PSession::pushFrame(const uint8_t* data, int len,
                           uint64_t timestampMs, bool keyframe) {
    m_sender.pushFrame(data, len, timestampMs, keyframe);
}

void P2PSession::stopSession() {
    /* 先停发送管线（join 线程后 conn 不再被触碰），再销毁连接 */
    m_sender.stop();
    destroyConnection();
    m_sessionActive.store(false);
}

bool P2PSession::isSessionActive() const {
    return m_sessionActive.load();
}

bool P2PSession::isPeerClosed() const {
    return m_peer_closed.load();
}

void P2PSession::destroyConnection() {
    if (m_conn) {
        delete m_conn;
        m_conn = nullptr;
    }
}

/* ====================================================================
 *  metaRTC 回调实现
 * ==================================================================== */

void P2PSession::onIceCandidate(int32_t uid, char* candidate) {
    if (candidate == nullptr) return;
    NC_LOGD("[P2PSession] local candidate (uid={}): %.80s", uid, candidate);
    /* gather 线程上下文：仅做字符串拷贝与信令转发，禁止阻塞操作 */
    if (m_candHandler) {
        m_candHandler(std::string(candidate));
    }
}

void P2PSession::onIceStateChange(int32_t uid, YangIceCandidateState iceState) {
    NC_LOGD("[P2PSession] ice state (uid={}): {}", uid, (int)iceState);
}

void P2PSession::onConnectionStateChange(int32_t uid, YangRtcConnectionState state) {
    NC_LOGI("[P2PSession] connection state (uid={}): {}", uid, (int)state);
}

void P2PSession::onIceGatheringState(int32_t uid, YangIceGatheringState gatherState) {
    NC_LOGD("[P2PSession] gathering state (uid={}): {}", uid, (int)gatherState);
}

void P2PSession::setMediaConfig(int32_t uid, YangAudioParam* audio, YangVideoParam* video) {
    (void)uid; (void)audio; (void)video;
}

void P2PSession::sendRequest(int32_t uid, uint32_t ssrc, YangRequestType req) {
    NC_LOGD("[P2PSession] rtc request (uid={} ssrc={} req={})", uid, ssrc, (int)req);
    /* PLI/关键帧请求接到相机 requestKeyFrame（回调线程内只转发，实际
     * ioctl 由相机库处理，无阻塞风险） */
    if (req == Yang_Req_Sendkeyframe && m_keyFrameHandler) {
        m_keyFrameHandler();
    }
}

void P2PSession::sslCloseAlert(int32_t uid) {
    NC_LOGI("[P2PSession] ssl close alert (uid={})", uid);
    /* 仅置标志：本回调运行在 metaRTC 接收线程，直接 stopSession 会
     * join 发送线程并销毁连接对象，存在自锁/自毁风险；
     * 由服务线程轮询 isPeerClosed 后在安全上下文终止会话 */
    m_peer_closed.store(true);
}

} // namespace rtc
