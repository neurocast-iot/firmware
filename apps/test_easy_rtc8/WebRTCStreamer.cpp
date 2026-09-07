/**
 * WebRTCStreamer.cpp - WebRTC WHIP 推流器实现（metaRTC 8.0 版本）
 *
 * 与 7.0 版本的关键差异：
 *   1. YangPeerConnection7 → YangPeerConnection8
 *   2. on_video(YangFrame*) → on_video(YangPushData*)，通过 YangRtcPacer 转换
 *   3. 数据格式: 直接传 Annex-B（含起始码）给 Pacer，由其内部处理 SPS/PPS 提取和 NAL 拆分
 */
#include "WebRTCStreamer.h"

#include <yangrtc/YangWhip.h>
#include <yangrtc/YangPeerInfo.h>
#include <yangrtc/YangPeerConnection8.h>
#include <yangvideo/YangMeta.h>
#include <yangvideo/YangNalu.h>
#include <yangutil/yangavinfo.h>
#include <yangutil/sys/YangTime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * 单例指针（用于 C 回调访问实例）
 */
WebRTCStreamer* WebRTCStreamer::s_instance = nullptr;

/* ====================================================================
 *  构造/析构
 * ==================================================================== */

WebRTCStreamer::WebRTCStreamer()
    : m_initialized(false) {
    m_frame_queue = new FrameItem[FRAME_QUEUE_SIZE];
    printf("[WebRTCStreamer8] constructor (frame_queue=%d x %dKB = %dMB heap)\n",
           FRAME_QUEUE_SIZE, MAX_FRAME_SIZE / 1024,
           (FRAME_QUEUE_SIZE * MAX_FRAME_SIZE) / (1024 * 1024));
}

WebRTCStreamer::~WebRTCStreamer() {
    cleanup();
    printf("[WebRTCStreamer8] destructor\n");
}

/* ====================================================================
 *  Public API
 * ==================================================================== */

bool WebRTCStreamer::init(const WebRTCConfig& config) {
    if (!config.isValid()) {
        printf("[WebRTCStreamer8] ERROR: config invalid\n");
        return false;
    }

    printf("[WebRTCStreamer8] init: %dx%d@%d %dkbps port=%d url=%s\n",
           config.width, config.height, config.fps, config.bitrate,
           config.rtcLocalPort, config.whipUrl.c_str());

    m_config = config;
    m_initialized = true;
    return true;
}

bool WebRTCStreamer::startPush(const std::string& whipUrl) {
    if (!m_initialized) {
        printf("[WebRTCStreamer8] ERROR: not initialized\n");
        return false;
    }

    if (m_pushing.load()) {
        printf("[WebRTCStreamer8] WARN: already pushing\n");
        return false;
    }

    printf("[WebRTCStreamer8] startPush: %s\n", whipUrl.c_str());

    /* ---- 阶段0: 复位上次推流残留状态（幂等保障：stop→start 重推不受旧时间基准/陈旧帧影响）---- */
    m_first_hw_ts = 0;
    m_q_write = 0;
    m_q_read = 0;
    m_q_count = 0;
    m_q_dropped = 0;

    /* ---- 阶段1: 初始化 YangAVInfo ---- */
    YangAVInfo avinfo;
    memset(&avinfo, 0, sizeof(YangAVInfo));
    avinfo.sys.mediaServer = Yang_Server_Whip_Whep;
    avinfo.audio.sample = m_config.audioSample;
    avinfo.audio.channel = m_config.audioChannel;
    avinfo.audio.audioEncoderType = Yang_AED_OPUS;
    avinfo.audio.enableAudioFec = yangfalse;
    avinfo.video.width = m_config.width;
    avinfo.video.height = m_config.height;
    avinfo.video.outWidth = m_config.width;
    avinfo.video.outHeight = m_config.height;
    avinfo.video.rate = m_config.bitrate;   /* kbps，供 SDP/带宽估计使用，实际编码码率由 VENC 控制 */
    avinfo.video.frame = m_config.fps;
    avinfo.video.videoCacheNum = 10;
    avinfo.video.evideoCacheNum = 10;
    avinfo.video.videoPlayCacheNum = 10;
    avinfo.video.videoEncoderType = Yang_VED_H264;
    avinfo.rtc.rtcLocalPort = m_config.rtcLocalPort;
    /* ICE 候选策略由配置传入: 0=Host 1=Stun 2=Turn（枚举值与 YangIceCandidateType 对齐） */
    avinfo.rtc.iceCandidateType = m_config.iceCandidateType;
    /* ICE 字段使用 snprintf 防溢出（目标缓冲区为定长数组） */
    snprintf(avinfo.rtc.iceServerIP, sizeof(avinfo.rtc.iceServerIP), "%s", m_config.iceServerIP.c_str());
    avinfo.rtc.iceServerPort = m_config.iceServerPort;
    snprintf(avinfo.rtc.iceUserName, sizeof(avinfo.rtc.iceUserName), "%s", m_config.iceUserName.c_str());
    snprintf(avinfo.rtc.icePassword, sizeof(avinfo.rtc.icePassword), "%s", m_config.icePassword.c_str());
    avinfo.enc.enc_threads = 4;

    /* ---- 阶段2: 创建 PeerConnection8（8.0）---- */
    YangPeerInfo peerInfo;
    yang_avinfo_initPeerInfo(&peerInfo, &avinfo);
    peerInfo.direction = YangSendonly;

    m_conn = new YangPeerConnection8(&peerInfo, NULL, NULL, NULL, NULL);
    m_conn->addAudioTrack(Yang_AED_OPUS);
    m_conn->addVideoTrack(Yang_VED_H264);
    m_conn->addTransceiver(YangMediaAudio, peerInfo.direction);
    m_conn->addTransceiver(YangMediaVideo, peerInfo.direction);

    /* ---- 阶段2.5: 初始化 YangRtcPacer（8.0 新增）---- */
    m_pacer = new YangRtcPacer();
    m_pacer->initVideo(Yang_VED_H264, 1024);
    m_pacer->initAudio(Yang_AED_OPUS, m_config.audioSample, m_config.audioChannel);

    /* ---- 阶段3: WHIP 连接 SRS ---- */
    uint64_t whip_start_ms = yang_get_system_time() / 1000;
    int ret = yang_whip_connectWhipWhepServer(&m_conn->m_peer, (char*)whipUrl.c_str());
    uint64_t whip_elapsed = yang_get_system_time() / 1000 - whip_start_ms;

    printf("[WebRTCStreamer8] WHIP connect: ret=%d, elapsed=%llums\n", ret, whip_elapsed);
    if (ret) {
        printf("[WebRTCStreamer8] ERROR: WHIP connect failed ret=%d\n", ret);
        delete m_pacer;
        m_pacer = nullptr;
        delete m_conn;
        m_conn = nullptr;
        return false;
    }

    /* ---- 阶段4: 等待 WebRTC 底层就绪（ICE/DTLS/SRTP）----
     * 事件驱动轮询 isConnected()，就绪即放行，替代调试期的固定 sleep(6)，
     * 推流启动时间从 ~7.5s 降至 ~2s；超时 10s 判定失败并清理退出。
     */
    int wait_ms = 0;
    while (!m_conn->isConnected() && wait_ms < 10000) {
        usleep(100 * 1000);
        wait_ms += 100;
    }
    if (!m_conn->isConnected()) {
        printf("[WebRTCStreamer8] ERROR: WebRTC not connected after %dms (ICE/DTLS/SRTP timeout)\n", wait_ms);
        delete m_pacer;
        m_pacer = nullptr;
        delete m_conn;
        m_conn = nullptr;
        return false;
    }
    printf("[WebRTCStreamer8] WebRTC connected (ICE/DTLS/SRTP ready, waited %dms)\n", wait_ms);

    /* ---- 阶段5: 启动发送线程 ---- */
    m_sender_running.store(true);
    pthread_create(&m_sender_thread, NULL, senderThreadFunc, this);
    printf("[WebRTCStreamer8] sender thread started\n");

    /* ---- 阶段6: 设置单例指针 ---- */
    s_instance = this;
    m_pushing.store(true);

    printf("[WebRTCStreamer8] push started\n");
    return true;
}

bool WebRTCStreamer::stopPush() {
    if (!m_pushing.load()) {
        printf("[WebRTCStreamer8] WARN: not pushing\n");
        return false;
    }

    printf("[WebRTCStreamer8] stopPush...\n");

    m_pushing.store(false);

    /* 停止发送线程 */
    m_sender_running.store(false);
    pthread_cond_signal(&m_q_cond);
    pthread_join(m_sender_thread, NULL);
    printf("[WebRTCStreamer8] sender thread stopped (dropped=%d)\n", m_q_dropped);

    /* 释放 8.0 新增资源 */
    if (m_pacer) {
        delete m_pacer;
        m_pacer = nullptr;
    }
    if (m_conn) {
        delete m_conn;
        m_conn = nullptr;
    }

    s_instance = nullptr;

    printf("[WebRTCStreamer8] push stopped\n");
    return true;
}

bool WebRTCStreamer::isPushing() const {
    return m_pushing.load();
}

void WebRTCStreamer::cleanup() {
    if (m_pushing.load()) {
        stopPush();
    }

    if (m_conn) {
        delete m_conn;
        m_conn = nullptr;
    }

    if (m_pacer) {
        delete m_pacer;
        m_pacer = nullptr;
    }

    if (m_frame_queue) {
        delete[] m_frame_queue;
        m_frame_queue = nullptr;
    }

    m_initialized = false;
    printf("[WebRTCStreamer8] cleanup done\n");
}

WebRTCConfig WebRTCStreamer::getActiveConfig() const {
    return m_config;
}

bool WebRTCStreamer::updateConfig(const WebRTCConfig& config) {
    if (m_pushing.load()) {
        printf("[WebRTCStreamer8] WARN: cannot update config while pushing\n");
        return false;
    }

    printf("[WebRTCStreamer8] updateConfig: %dx%d@%d %dkbps\n",
           config.width, config.height, config.fps, config.bitrate);

    m_config = config;
    m_initialized = config.isValid();
    return true;
}

/* ====================================================================
 *  帧回调（C 风格，由 MainChannelDispatcher 调用）
 * ==================================================================== */

void WebRTCStreamer::frameCallback(unsigned char* data, int len,
                                   unsigned long long timestamp, int is_keyframe) {
    if (!s_instance || !s_instance->m_pushing.load()) return;
    if (!data || len <= 0 || len > MAX_FRAME_SIZE) return;

    WebRTCStreamer* me = s_instance;

    if (me->m_first_hw_ts == 0) {
        me->m_first_hw_ts = timestamp;
    }

    unsigned long long hw_ts_ms = timestamp - me->m_first_hw_ts;

    /* 入队 */
    pthread_mutex_lock(&me->m_q_lock);
    if (me->m_q_count < FRAME_QUEUE_SIZE) {
        FrameItem& item = me->m_frame_queue[me->m_q_write];
        memcpy(item.data, data, len);
        item.len = len;
        /*
         * PTS 单位: 微秒（metaRTC 内部约定）
         *   Pacer 的 YangTimestamp 会执行 ×9/100 完成 µs→90kHz 转换，
         *   此处不可预先转换为 90kHz，否则二次转换导致时间轴压缩（幻灯片效果）
         */
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

void (*WebRTCStreamer::getFrameCallback())(unsigned char*, int, unsigned long long, int) {
    return frameCallback;
}

/* ====================================================================
 *  发送线程（8.0 版本）
 *
 *  流程: 帧队列取帧 → 构建 YangFrame（Annex-B 直通）
 *        → Pacer 转换（内部完成 SPS/PPS 提取、起始码处理、RTP 打包）
 *        → on_video(YangPushData)
 * ==================================================================== */

void* WebRTCStreamer::senderThreadFunc(void* arg) {
    WebRTCStreamer* me = (WebRTCStreamer*)arg;
    int sent_frame_count = 0;
    int sent_i_count = 0;
    int sent_p_count = 0;

    /* 8.0 新增：YangFrame 和 YangPushData */
    YangFrame videoFrame;
    YangPushData videoData;
    memset(&videoFrame, 0, sizeof(YangFrame));
    memset(&videoData, 0, sizeof(YangPushData));

    printf("[WebRTCStreamer8::sender] started (8.0 API, Annex-B passthrough)\n");
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

        /*
         * 构建 YangFrame（Annex-B 原始数据，含 00 00 00 01 起始码）
         *
         * Pacer 内部 yang_pushVideo_getData() 依赖 yang_find_pre_start_code()
         * 搜索起始码来定位 SPS/PPS/IDR，必须传入 Annex-B 格式
         */
        memset(&videoFrame, 0, sizeof(YangFrame));
        videoFrame.payload = item.data;
        videoFrame.nb = item.len;
        videoFrame.pts = item.pts;
        videoFrame.frametype = item.is_keyframe ? YANG_Frametype_I : YANG_Frametype_P;
        videoFrame.uid = 0;

        /* 通过 Pacer 转换并推送到 MetaRTC 8.0 */
        YangPushData* pushData = me->m_pacer->getVideoData(&videoFrame);
        if (pushData) {
            me->m_conn->on_video(pushData);
        } else if (sent_frame_count <= 3) {
            /* 前3帧输出诊断日志，便于排查 Pacer 返回 NULL */
            printf("[WebRTCStreamer8][DBG] pacer returned NULL: frame=%d, type=%d, nb=%d\n",
                   sent_frame_count, videoFrame.frametype, videoFrame.nb);
        }

        if (item.is_keyframe) sent_i_count++;
        else sent_p_count++;

        if (sent_frame_count % 100 == 0) {
            printf("[WebRTCStreamer8::sender] %d frames (I=%d P=%d) q=%d\n",
                   sent_frame_count, sent_i_count, sent_p_count, me->m_q_count);
            fflush(stdout);
        }
    }

    printf("[WebRTCStreamer8::sender] stopped: total=%d I=%d P=%d\n",
           sent_frame_count, sent_i_count, sent_p_count);
    fflush(stdout);
    return NULL;
}
