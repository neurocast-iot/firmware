/**
 * WebRTCStreamer.h - WebRTC WHIP 推流器头文件（metaRTC 8.0 版本）
 *
 * 功能：
 *   - 封装 MetaRTC 8.0 库的 WebRTC WHIP 推流功能
 *   - 专注推流逻辑，不管理相机设备
 *   - 对标 RTMPStreamer 设计，适配 MainChannelDispatcher 回调注册
 *
 * 与 7.0 版本的关键差异：
 *   - YangPeerConnection7 → YangPeerConnection8
 *   - on_video(YangFrame*) → on_video(YangPushData*)，通过 YangRtcPacer 转换
 *   - yang_createH264Meta → yang_meta_createH264（新 Meta API）
 *   - yang_parseH264Nalu → yang_nalu_getH264KeyframePos（新 NAL 解析 API）
 *   - YangVideoMeta → YangH2645Conf（存储 SPS/PPS/VPS）
 */
#ifndef WEBRTC_STREAMER8_H
#define WEBRTC_STREAMER8_H

#include <string>
#include <atomic>
#include <pthread.h>

class YangPeerConnection8;
class YangRtcPacer;
#include <yangutil/yangavinfo.h>

/**
 * WebRTCConfig - WebRTC WHIP 推流专用配置结构体
 */
struct WebRTCConfig {
    /* 推流开关 */
    bool enabled = false;

    /* WHIP 推流地址 */
    std::string whipUrl = "";

    /* 视频参数（无默认值，必须由调用方显式传入，isValid() 强制校验） */
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate = 0;

    /* RTC 参数 */
    int rtcLocalPort = 17000;

    /* 音频参数（当前仅参与 SDP 协商，WebRTC 标准为 Opus 48kHz 立体声） */
    int audioSample  = 48000;
    int audioChannel = 2;

    /* ICE 候选策略: 0=Host(局域网直连) 1=Stun 2=Turn，为 4G 推公网预留 */
    int iceCandidateType = 0;

    /* ICE/STUN/TURN 服务器参数 */
    std::string iceServerIP = "";
    int iceServerPort = 3478;
    std::string iceUserName = "";
    std::string icePassword = "";

    bool isValid() const {
        return enabled && !whipUrl.empty() && width > 0 && height > 0 && fps > 0 && bitrate > 0;
    }

    bool operator==(const WebRTCConfig& other) const {
        return enabled == other.enabled
            && whipUrl == other.whipUrl
            && width == other.width
            && height == other.height
            && fps == other.fps
            && bitrate == other.bitrate
            && rtcLocalPort == other.rtcLocalPort
            && audioSample == other.audioSample
            && audioChannel == other.audioChannel
            && iceCandidateType == other.iceCandidateType
            && iceServerIP == other.iceServerIP
            && iceServerPort == other.iceServerPort
            && iceUserName == other.iceUserName
            && icePassword == other.icePassword;
    }

    bool operator!=(const WebRTCConfig& other) const {
        return !(*this == other);
    }
};

/**
 * WebRTCStreamer 类（metaRTC 8.0 版本）
 *
 * 与 7.0 版本差异：
 *   - m_conn 类型: YangPeerConnection8
 *   - 新增 m_pacer: YangRtcPacer（YangFrame → YangPushData 转换）
 *   - m_vmd 类型: YangH2645Conf（替代 YangVideoMeta）
 */
class WebRTCStreamer {
public:
    WebRTCStreamer();
    ~WebRTCStreamer();

    /**
     * 初始化 WebRTC 推流器
     * @param config WebRTC 推流配置
     */
    bool init(const WebRTCConfig& config);

    /**
     * 开始 WebRTC WHIP 推流
     * @param whipUrl WHIP 服务器地址
     */
    bool startPush(const std::string& whipUrl);

    /**
     * 停止 WebRTC WHIP 推流
     */
    bool stopPush();

    /**
     * 检查推流状态
     */
    bool isPushing() const;

    /**
     * 清理所有资源
     */
    void cleanup();

    /**
     * 视频帧回调函数（C 风格，供 MainChannelDispatcher 注册）
     */
    static void frameCallback(unsigned char* data, int len,
                              unsigned long long timestamp, int is_keyframe);

    /**
     * 获取帧回调函数指针
     */
    static void (*getFrameCallback())(unsigned char*, int, unsigned long long, int);

    WebRTCConfig getActiveConfig() const;
    bool updateConfig(const WebRTCConfig& config);

private:
    /* ===== 帧队列常量 ===== */
    static const int FRAME_QUEUE_SIZE = 32;
    static const int MAX_FRAME_SIZE = 256 * 1024;

    /* ===== 帧队列结构体 ===== */
    struct FrameItem {
        unsigned char data[MAX_FRAME_SIZE];
        int len;
        int64_t pts;       /* PTS（微秒，metaRTC 内部转 90kHz） */
        int is_keyframe;
    };

    /* ===== 帧队列(生产者-消费者) — 动态分配避免栈溢出 ===== */
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
    YangPeerConnection8* m_conn = nullptr;   /* 8.0: PeerConnection8 */
    YangRtcPacer* m_pacer = nullptr;         /* 8.0 新增: 帧转换器（内部处理 SPS/PPS 提取） */

    /* ===== 状态 ===== */
    bool m_initialized = false;
    std::atomic<bool> m_pushing{false};
    WebRTCConfig m_config;

    /* ===== 单例指针（供静态 C 回调访问实例） ===== */
    static WebRTCStreamer* s_instance;

    /* ──────── 发送线程 ──────── */

    /**
     * 发送线程入口（8.0 版本）
     *
     * 从帧队列取帧 → 构建 YangFrame（Annex-B 直通）
     * → Pacer 转换（内部完成 SPS/PPS 提取、起始码处理、RTP 打包）
     * → on_video(YangPushData)
     */
    static void* senderThreadFunc(void* arg);
};

#endif /* WEBRTC_STREAMER8_H */
