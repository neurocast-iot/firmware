/**
 * @file frame_sender.h
 * @brief 共用帧发送管线：帧队列 + 发送线程 + YangRtcPacer 转换
 *
 * 从 test_p2p_rtc8/P2PStreamer 与 test_easy_rtc8/WebRTCStreamer 中抽取的
 * 完全同构代码（两工程的帧队列/发送线程逐行一致，已上板验证），
 * 抽出后 P2PSession / WhipPusher 各自只剩连接管理。
 *
 * 与测试工程的差异：去掉静态单例 s_instance + C 风格静态回调模式，
 * 改为实例方法 pushFrame()——mediad 内由 LiveStreamService 统一订阅
 * 相机帧后分发到当前活跃会话，不再需要 C 回调注册。
 *
 * 数据格式约束（metaRTC 8.0）：
 *   - 帧必须为 Annex-B 直通（含 00 00 00 01 起始码），Pacer 内部
 *     yang_pushVideo_getData 依赖起始码定位 SPS/PPS/IDR
 *   - PTS 单位微秒：Pacer 的 YangTimestamp 执行 ×9/100 转 90kHz，
 *     禁止预转换为 90kHz（否则二次转换导致时间轴压缩、幻灯片效果）
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <pthread.h>

class YangPeerConnection8;
class YangRtcPacer;
#include <yangrtc/YangPeerConnection8.h>

namespace rtc {

class FrameSender {
public:
    FrameSender();
    ~FrameSender();

    FrameSender(const FrameSender&) = delete;
    FrameSender& operator=(const FrameSender&) = delete;

    /**
     * 启动发送管线：复位队列/时间基准 → 创建 Pacer → 拉起发送线程
     *
     * @param conn         已完成 ICE/DTLS/SRTP 协商的连接（生命周期由调用方
     *                     保证覆盖到 stop() 返回之后）
     * @param audioSample  音频采样率（仅初始化 Pacer 音频轨，当前不推音频帧）
     * @param audioChannel 音频通道数
     * @return true=线程已启动（重复调用返回 false）
     */
    bool start(YangPeerConnection8* conn, int audioSample, int audioChannel);

    /** 停止发送线程并释放 Pacer（幂等；返回后 conn 不再被本类触碰） */
    void stop();

    bool isRunning() const { return m_running.load(); }

    /**
     * 入队一帧（相机采集线程调用；未 start 时直接丢弃，队满丢帧计数）
     *
     * @param data        Annex-B 帧数据（回调返回后即可失效，内部拷贝）
     * @param len         帧长度（超过 256KB 丢弃）
     * @param timestampMs 硬件时间戳（毫秒，内部以首帧为基准转微秒 PTS）
     * @param keyframe    是否 I 帧
     */
    void pushFrame(const uint8_t* data, int len, uint64_t timestampMs, bool keyframe);

private:
    /* ===== 帧队列常量（与两个测试工程一致，已验证容量） ===== */
    static const int FRAME_QUEUE_SIZE = 32;
    static const int MAX_FRAME_SIZE = 256 * 1024;

    struct FrameItem {
        unsigned char data[MAX_FRAME_SIZE];
        int len;
        int64_t pts;            /* 微秒（metaRTC 内部转 90kHz） */
        int is_keyframe;
    };

    static void* senderThreadFunc(void* arg);

    /* ===== 帧队列(生产者-消费者) — 堆分配避免栈溢出（32×256KB=8MB） ===== */
    FrameItem* m_frame_queue = nullptr;
    int m_q_write = 0;
    int m_q_read = 0;
    int m_q_count = 0;
    int m_q_dropped = 0;
    pthread_mutex_t m_q_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t m_q_cond = PTHREAD_COND_INITIALIZER;
    pthread_t m_sender_thread;
    std::atomic<bool> m_running{false};

    /* ===== 首帧时间基准 ===== */
    unsigned long long m_first_hw_ts = 0;

    /* ===== 输出目标（不持有所有权） ===== */
    YangPeerConnection8* m_conn = nullptr;
    YangRtcPacer* m_pacer = nullptr;
};

} // namespace rtc
