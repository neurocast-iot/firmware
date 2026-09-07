/**
 * @file frame_sender.cpp
 * @brief 共用帧发送管线实现（自 P2PStreamer/WebRTCStreamer 同构代码原样抽取）
 *
 * 抽取范围（两工程逐行一致的部分）：
 *   - 帧队列入队逻辑（frameCallback → pushFrame，去掉静态单例）
 *   - 发送线程（senderThreadFunc：取帧 → YangFrame → Pacer → on_video）
 *   - Pacer 生命周期（initVideo H264 1024 / initAudio）
 */
#include "rtc/frame_sender.h"

#include <nc/common/log_utils.h>

#include <cstring>

namespace rtc {

/* static const 成员外部定义（C++11 前向兼容） */
constexpr int FrameSender::FRAME_QUEUE_SIZE;
constexpr int FrameSender::MAX_FRAME_SIZE;

FrameSender::FrameSender() {
    m_frame_queue = new FrameItem[FRAME_QUEUE_SIZE];
    NC_LOGD("[FrameSender] created (frame_queue={} x {}KB = {}MB heap)",
            FRAME_QUEUE_SIZE, MAX_FRAME_SIZE / 1024,
            (FRAME_QUEUE_SIZE * MAX_FRAME_SIZE) / (1024 * 1024));
}

FrameSender::~FrameSender() {
    stop();
    delete[] m_frame_queue;
    m_frame_queue = nullptr;
}

bool FrameSender::start(YangPeerConnection8* conn, int audioSample, int audioChannel) {
    if (conn == nullptr || m_running.load()) return false;

    /* 复位上次会话残留（幂等保障：重启会话不受旧时间基准/陈旧帧影响） */
    m_first_hw_ts = 0;
    m_q_write = 0;
    m_q_read = 0;
    m_q_count = 0;
    m_q_dropped = 0;

    m_conn = conn;
    m_pacer = new YangRtcPacer();
    m_pacer->initVideo(Yang_VED_H264, 1024);
    m_pacer->initAudio(Yang_AED_OPUS, audioSample, audioChannel);

    m_running.store(true);
    pthread_create(&m_sender_thread, NULL, senderThreadFunc, this);
    NC_LOGI("[FrameSender] sender thread started");
    return true;
}

void FrameSender::stop() {
    if (m_running.load()) {
        m_running.store(false);
        pthread_cond_signal(&m_q_cond);
        pthread_join(m_sender_thread, NULL);
        NC_LOGI("[FrameSender] sender thread stopped (dropped={})", m_q_dropped);
    }
    if (m_pacer) {
        delete m_pacer;
        m_pacer = nullptr;
    }
    m_conn = nullptr;
}

void FrameSender::pushFrame(const uint8_t* data, int len,
                            uint64_t timestampMs, bool keyframe) {
    if (!m_running.load()) return;
    if (data == nullptr || len <= 0 || len > MAX_FRAME_SIZE) return;

    if (m_first_hw_ts == 0) {
        m_first_hw_ts = timestampMs;
    }
    unsigned long long hw_ts_ms = timestampMs - m_first_hw_ts;

    pthread_mutex_lock(&m_q_lock);
    if (m_q_count < FRAME_QUEUE_SIZE) {
        FrameItem& item = m_frame_queue[m_q_write];
        memcpy(item.data, data, len);
        item.len = len;
        /* PTS 单位: 微秒（Pacer 内部 ×9/100 转 90kHz，禁止预转换） */
        item.pts = (int64_t)hw_ts_ms * 1000;
        item.is_keyframe = keyframe ? 1 : 0;
        m_q_write = (m_q_write + 1) % FRAME_QUEUE_SIZE;
        m_q_count++;
        pthread_cond_signal(&m_q_cond);
    } else {
        m_q_dropped++;
    }
    pthread_mutex_unlock(&m_q_lock);
}

void* FrameSender::senderThreadFunc(void* arg) {
    FrameSender* me = (FrameSender*)arg;
    int sent_frame_count = 0;
    int sent_i_count = 0;
    int sent_p_count = 0;

    YangFrame videoFrame;
    memset(&videoFrame, 0, sizeof(YangFrame));

    NC_LOGI("[FrameSender::sender] started (Annex-B passthrough)");

    while (me->m_running.load()) {
        pthread_mutex_lock(&me->m_q_lock);
        while (me->m_q_count == 0 && me->m_running.load()) {
            pthread_cond_wait(&me->m_q_cond, &me->m_q_lock);
        }
        if (!me->m_running.load()) {
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
            /* 前3帧输出诊断日志，便于排查 Pacer 返回 NULL */
            NC_LOGW("[FrameSender] pacer returned NULL: frame={}, type={}, nb={}",
                    sent_frame_count, videoFrame.frametype, videoFrame.nb);
        }

        if (item.is_keyframe) sent_i_count++;
        else sent_p_count++;

        if (sent_frame_count % 300 == 0) {
            NC_LOGD("[FrameSender::sender] {} frames (I={} P={}) q={}",
                    sent_frame_count, sent_i_count, sent_p_count, me->m_q_count);
        }
    }

    NC_LOGI("[FrameSender::sender] stopped: total={} I={} P={}",
            sent_frame_count, sent_i_count, sent_p_count);
    return NULL;
}

} // namespace rtc
