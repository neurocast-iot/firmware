/**
 * P2PStreamer.cpp - P2P 推流器实现（设备端，answer 方，metaRTC 8.0）
 *
 * 与 WebRTCStreamer 的关键差异：
 *   1. 信令：WHIP HTTP → offer/answer/candidate 由外部信令层搬运
 *   2. 角色：offer 方 → answer 方（isControlled=true，DTLS passive）
 *   3. ICE：YangIceModeFull + enableSdpCandidate，gather 线程真实执行，
 *      STUN/TURN 按 iceCandidateType 原生生效
 *   4. 回调：构造时注入 YangCallbackIce（WHIP 路径传 NULL），
 *      onIceCandidate 产出 Trickle 候选交由上层信令外发
 */
#include "P2PStreamer.h"

#include <yangrtc/YangPeerInfo.h>
#include <yangutil/yangavinfo.h>
#include <yangutil/sys/YangTime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

P2PStreamer* P2PStreamer::s_instance = nullptr;

/* ====================================================================
 *  构造/析构
 * ==================================================================== */

P2PStreamer::P2PStreamer() {
    m_frame_queue = new FrameItem[FRAME_QUEUE_SIZE];
    printf("[P2PStreamer] constructor (frame_queue=%d x %dKB = %dMB heap)\n",
           FRAME_QUEUE_SIZE, MAX_FRAME_SIZE / 1024,
           (FRAME_QUEUE_SIZE * MAX_FRAME_SIZE) / (1024 * 1024));
}

P2PStreamer::~P2PStreamer() {
    stopSession();
    if (m_frame_queue) {
        delete[] m_frame_queue;
        m_frame_queue = nullptr;
    }
    printf("[P2PStreamer] destructor\n");
}

/* ====================================================================
 *  Public API
 * ==================================================================== */

bool P2PStreamer::init(const P2PConfig& config) {
    if (!config.isValid()) {
        printf("[P2PStreamer] ERROR: config invalid\n");
        return false;
    }
    printf("[P2PStreamer] init: %dx%d@%d %dkbps port=%d iceType=%d iceServer=%s:%d\n",
           config.width, config.height, config.fps, config.bitrate,
           config.rtcLocalPort, config.iceCandidateType,
           config.iceServerIP.c_str(), config.iceServerPort);
    m_config = config;
    m_initialized = true;
    return true;
}

void P2PStreamer::setCandidateHandler(CandidateHandler handler) {
    m_candHandler = handler;
}

bool P2PStreamer::handleOffer(const std::string& offerSdp, std::string& answerSdp) {
    if (!m_initialized) {
        printf("[P2PStreamer] ERROR: not initialized\n");
        return false;
    }

    /* ---- 阶段0: 幂等保障——清理上一会话残留（连接/线程/队列/时间基准） ---- */
    stopSession();
    m_peer_closed.store(false);   /* 新会话复位对端关闭标志 */
    m_first_hw_ts = 0;
    m_q_write = 0;
    m_q_read = 0;
    m_q_count = 0;
    m_q_dropped = 0;

    /* ---- 阶段1: 构建 YangAVInfo → YangPeerInfo（对齐 WebRTCStreamer 参数装配） ---- */
    YangAVInfo avinfo;
    memset(&avinfo, 0, sizeof(YangAVInfo));
    avinfo.audio.sample = m_config.audioSample;
    avinfo.audio.channel = m_config.audioChannel;
    avinfo.audio.audioEncoderType = Yang_AED_OPUS;
    avinfo.audio.enableAudioFec = yangfalse;
    avinfo.video.width = m_config.width;
    avinfo.video.height = m_config.height;
    avinfo.video.outWidth = m_config.width;
    avinfo.video.outHeight = m_config.height;
    avinfo.video.rate = m_config.bitrate;
    avinfo.video.frame = m_config.fps;
    avinfo.video.videoCacheNum = 10;
    avinfo.video.evideoCacheNum = 10;
    avinfo.video.videoPlayCacheNum = 10;
    avinfo.video.videoEncoderType = Yang_VED_H264;
    avinfo.rtc.rtcLocalPort = m_config.rtcLocalPort;
    avinfo.rtc.iceCandidateType = m_config.iceCandidateType;
    snprintf(avinfo.rtc.iceServerIP, sizeof(avinfo.rtc.iceServerIP), "%s", m_config.iceServerIP.c_str());
    avinfo.rtc.iceServerPort = m_config.iceServerPort;
    snprintf(avinfo.rtc.iceUserName, sizeof(avinfo.rtc.iceUserName), "%s", m_config.iceUserName.c_str());
    snprintf(avinfo.rtc.icePassword, sizeof(avinfo.rtc.icePassword), "%s", m_config.icePassword.c_str());
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

    /* TURN 模式：Allocate 等待窗口默认仅 1000ms（YangAvtype.c L169，
     * initTurn 折算 50次×20ms 轮询），在 CGNAT 链路上余量不足；
     * 放宽到 5s（仍远小于 sessionTimeout 30s），成功时循环提前退出不增加延迟 */
    if (m_config.iceCandidateType == 2) {
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
        printf("[P2PStreamer] ERROR: setRemoteDescription failed ret=%d\n", ret);
        destroyConnection();
        return false;
    }

    char* answerBuf = (char*)calloc(1, ANSWER_SDP_SIZE);
    ret = m_conn->createAnswer(answerBuf);
    if (ret != 0 || answerBuf[0] == '\0') {
        printf("[P2PStreamer] ERROR: createAnswer failed ret=%d\n", ret);
        free(answerBuf);
        destroyConnection();
        return false;
    }
    answerSdp = answerBuf;

    /* metaRTC 在 enableSdpCandidate 开启时会把 answer 的 CRLF 预转义为字面文本 "\r\n"
     * （YangSdp.c yang_sdp_genLocalSdp2 L424），该格式仅适配其手工拼 JSON 的 demo 信令；
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
        printf("[P2PStreamer] ERROR: setLocalDescription failed ret=%d\n", ret);
        destroyConnection();
        return false;
    }

    /* ---- 阶段4: 初始化 Pacer（帧转换器） ---- */
    m_pacer = new YangRtcPacer();
    m_pacer->initVideo(Yang_VED_H264, 1024);
    m_pacer->initAudio(Yang_AED_OPUS, m_config.audioSample, m_config.audioChannel);

    m_sessionActive.store(true);
    printf("[P2PStreamer] session negotiating (answer generated, %zu bytes)\n", answerSdp.size());
    return true;
}

bool P2PStreamer::handleRemoteCandidate(const std::string& candidate) {
    if (m_conn == nullptr) {
        printf("[P2PStreamer] WARN: candidate dropped (no connection)\n");
        return false;
    }
    int ret = m_conn->addIceCandidate((char*)candidate.c_str());
    printf("[P2PStreamer] addIceCandidate ret=%d: %.80s\n", ret, candidate.c_str());
    return ret == 0;
}

bool P2PStreamer::waitConnected(int timeoutMs) {
    if (m_conn == nullptr) return false;

    int waited = 0;
    while (!m_conn->isConnected() && waited < timeoutMs) {
        usleep(100 * 1000);
        waited += 100;
    }
    if (!m_conn->isConnected()) {
        printf("[P2PStreamer] ERROR: not connected after %dms (ICE/DTLS/SRTP timeout)\n", waited);
        return false;
    }
    printf("[P2PStreamer] connected (ICE/DTLS/SRTP ready, waited %dms)\n", waited);
    return true;
}

bool P2PStreamer::isConnected() {
    return m_conn != nullptr && m_conn->isConnected();
}

bool P2PStreamer::startSending() {
    if (m_conn == nullptr || m_sender_running.load()) return false;

    m_sender_running.store(true);
    pthread_create(&m_sender_thread, NULL, senderThreadFunc, this);
    s_instance = this;
    printf("[P2PStreamer] sender thread started\n");
    return true;
}

void P2PStreamer::stopSession() {
    /* 先摘单例指针，阻断帧回调继续入队 */
    s_instance = nullptr;

    if (m_sender_running.load()) {
        m_sender_running.store(false);
        pthread_cond_signal(&m_q_cond);
        pthread_join(m_sender_thread, NULL);
        printf("[P2PStreamer] sender thread stopped (dropped=%d)\n", m_q_dropped);
    }

    destroyConnection();
    m_sessionActive.store(false);
}

bool P2PStreamer::isSessionActive() const {
    return m_sessionActive.load();
}

bool P2PStreamer::isPeerClosed() const {
    return m_peer_closed.load();
}

void P2PStreamer::destroyConnection() {
    if (m_pacer) {
        delete m_pacer;
        m_pacer = nullptr;
    }
    if (m_conn) {
        delete m_conn;
        m_conn = nullptr;
    }
}

/* ====================================================================
 *  帧回调（C 风格，与 WebRTCStreamer::frameCallback 相同的入队逻辑）
 * ==================================================================== */

void P2PStreamer::frameCallback(unsigned char* data, int len,
                                unsigned long long timestamp, int is_keyframe) {
    P2PStreamer* me = s_instance;
    if (me == nullptr || !me->m_sender_running.load()) return;
    if (!data || len <= 0 || len > MAX_FRAME_SIZE) return;

    if (me->m_first_hw_ts == 0) {
        me->m_first_hw_ts = timestamp;
    }
    unsigned long long hw_ts_ms = timestamp - me->m_first_hw_ts;

    pthread_mutex_lock(&me->m_q_lock);
    if (me->m_q_count < FRAME_QUEUE_SIZE) {
        FrameItem& item = me->m_frame_queue[me->m_q_write];
        memcpy(item.data, data, len);
        item.len = len;
        /* PTS 单位: 微秒（Pacer 内部 ×9/100 转 90kHz，禁止预转换） */
        item.pts = (int64_t)hw_ts_ms * 1000;
        item.is_keyframe = is_keyframe;
        me->m_q_write = (me->m_q_write + 1) % FRAME_QUEUE_SIZE;
        me->m_q_count++;
        pthread_cond_signal(&me->m_q_cond);
    } else {
        me->m_q_dropped++;
    }
    pthread_mutex_unlock(&me->m_q_lock);
}

/* ====================================================================
 *  发送线程（与 WebRTCStreamer::senderThreadFunc 相同的 Pacer 喂帧流程）
 * ==================================================================== */

void* P2PStreamer::senderThreadFunc(void* arg) {
    P2PStreamer* me = (P2PStreamer*)arg;
    int sent_frame_count = 0;
    int sent_i_count = 0;
    int sent_p_count = 0;

    YangFrame videoFrame;
    memset(&videoFrame, 0, sizeof(YangFrame));

    printf("[P2PStreamer::sender] started (Annex-B passthrough)\n");
    fflush(stdout);

    while (me->m_sender_running.load()) {
        pthread_mutex_lock(&me->m_q_lock);
        while (me->m_q_count == 0 && me->m_sender_running.load()) {
            pthread_cond_wait(&me->m_q_cond, &me->m_q_lock);
        }
        if (!me->m_sender_running.load()) {
            pthread_mutex_unlock(&me->m_q_lock);
            break;
        }

        FrameItem item = me->m_frame_queue[me->m_q_read];
        me->m_q_read = (me->m_q_read + 1) % FRAME_QUEUE_SIZE;
        me->m_q_count--;
        pthread_mutex_unlock(&me->m_q_lock);

        sent_frame_count++;

        /* Annex-B 原始数据直通（Pacer 依赖起始码定位 SPS/PPS/IDR） */
        memset(&videoFrame, 0, sizeof(YangFrame));
        videoFrame.payload = item.data;
        videoFrame.nb = item.len;
        videoFrame.pts = item.pts;
        videoFrame.frametype = item.is_keyframe ? YANG_Frametype_I : YANG_Frametype_P;
        videoFrame.uid = 0;

        YangPushData* pushData = me->m_pacer->getVideoData(&videoFrame);
        if (pushData) {
            me->m_conn->on_video(pushData);
        } else if (sent_frame_count <= 3) {
            printf("[P2PStreamer][DBG] pacer returned NULL: frame=%d, type=%d, nb=%d\n",
                   sent_frame_count, videoFrame.frametype, videoFrame.nb);
        }

        if (item.is_keyframe) sent_i_count++;
        else sent_p_count++;

        if (sent_frame_count % 100 == 0) {
            printf("[P2PStreamer::sender] %d frames (I=%d P=%d) q=%d\n",
                   sent_frame_count, sent_i_count, sent_p_count, me->m_q_count);
            fflush(stdout);
        }
    }

    printf("[P2PStreamer::sender] stopped: total=%d I=%d P=%d\n",
           sent_frame_count, sent_i_count, sent_p_count);
    fflush(stdout);
    return NULL;
}

/* ====================================================================
 *  metaRTC 回调实现
 * ==================================================================== */

void P2PStreamer::onIceCandidate(int32_t uid, char* candidate) {
    if (candidate == nullptr) return;
    printf("[P2PStreamer] local candidate (uid=%d): %.80s\n", uid, candidate);
    /* gather 线程上下文：仅做字符串拷贝与信令转发，禁止阻塞操作 */
    if (m_candHandler) {
        m_candHandler(std::string(candidate));
    }
}

void P2PStreamer::onIceStateChange(int32_t uid, YangIceCandidateState iceState) {
    printf("[P2PStreamer] ice state (uid=%d): %d\n", uid, (int)iceState);
}

void P2PStreamer::onConnectionStateChange(int32_t uid, YangRtcConnectionState state) {
    printf("[P2PStreamer] connection state (uid=%d): %d\n", uid, (int)state);
}

void P2PStreamer::onIceGatheringState(int32_t uid, YangIceGatheringState gatherState) {
    printf("[P2PStreamer] gathering state (uid=%d): %d\n", uid, (int)gatherState);
}

void P2PStreamer::setMediaConfig(int32_t uid, YangAudioParam* audio, YangVideoParam* video) {
    (void)uid; (void)audio; (void)video;
}

void P2PStreamer::sendRequest(int32_t uid, uint32_t ssrc, YangRequestType req) {
    printf("[P2PStreamer] rtc request (uid=%d ssrc=%u req=%d)\n", uid, ssrc, (int)req);
}

void P2PStreamer::sslCloseAlert(int32_t uid) {
    printf("[P2PStreamer] ssl close alert (uid=%d)\n", uid);
    /* 仅置标志：本回调运行在 metaRTC 接收线程，直接 stopSession 会
     * join 发送线程并销毁连接对象，存在自锁/自毁风险；
     * 由主循环轮询 isPeerClosed 后在安全上下文终止会话 */
    m_peer_closed.store(true);
}
