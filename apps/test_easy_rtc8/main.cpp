/**
 * test-easy-rtc8 — GC4653 硬件编码器 WebRTC WHIP 推流程序（metaRTC 8.0 版本）
 *
 * 架构：编码器回调(生产者) → 帧队列(环形缓冲) → 发送线程(消费者)
 *       发送线程处理：AVCC 转换 → Meta 推送 → NAL 解析 → 裸 NAL 推流
 *
 * 与 7.0 版本的关键差异：
 *   - YangPeerConnection7 → YangPeerConnection8
 *   - on_video(YangFrame*) → on_video(YangPushData*)，需通过 YangRtcPacer 转换
 *   - yang_createH264Meta → yang_meta_createH264（新 Meta API）
 *   - yang_parseH264Nalu → yang_nalu_getH264KeyframePos（新 NAL 解析 API）
 *
 * 参考：metaRTC-8.0-main/demo/metapushstream8/yangpush/YangRtcPublish.cpp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>

#include "camera/camera_device.h"
#include "camera/camera_config.h"
#include "camera/types.h"
#include "camera/error.h"

/* metaRTC 8.0 头文件 */
#include <yangrtc/YangWhip.h>
#include <yangrtc/YangPeerInfo.h>
#include <yangrtc/YangPeerConnection8.h>
#include <yangvideo/YangMeta.h>
#include <yangvideo/YangNalu.h>

/* ===== 全局状态 ===== */

static volatile sig_atomic_t g_running = 1;       /* Ctrl+C 退出标志 */

/* lib-carema-cpp CameraDevice（替换 vi_capture） */
static std::unique_ptr<camera::CameraDevice> g_cameraDevice;  /* 相机设备实例 */
static camera::SubscriptionId g_mainSubId = 0;                /* 主通道订阅 ID */

static YangPeerConnection8* g_conn = NULL;        /* MetaRTC PeerConnection (WHIP) */
static YangRtcPacer* g_pacer = NULL;              /* 8.0 新增：帧转换器 */
static YangH2645Conf g_h264Conf;                  /* 8.0 新增：H.264 配置（SPS/PPS/VPS） */

/* 编码器回调统计 (生产者侧) */
static int g_frame_count = 0;                       /* 累计接收帧数 */
static int g_i_frame_count = 0;                     /* I 帧计数 */
static int g_p_frame_count = 0;                     /* P 帧计数 */
static int g_total_bytes = 0;                       /* 累计数据量 */
static unsigned long long g_first_hw_ts = 0;       /* 首帧硬件时间戳(毫秒)，用于 PTS 归零 */
static struct timeval g_start_time = {0, 0};        /* 首帧的系统时间，用于 fps/kbps 计算 */

static int g_meta_extracted = 0;                    /* SPS/PPS meta 是否已从首帧提取 */

/* ===== 帧队列 (生产者-消费者) ===== */

#define FRAME_QUEUE_SIZE 32                         /* 环形队列容量 */
#define MAX_FRAME_SIZE   (256 * 1024)               /* 单帧最大 256KB */

struct FrameItem {
    unsigned char data[MAX_FRAME_SIZE];              /* 帧数据 */
    int len;                                         /* 数据长度 */
    int64_t pts;                                     /* 展示时间戳(微秒，metaRTC 内部转 90kHz) */
    int is_keyframe;                                 /* 是否关键帧 */
};

static FrameItem g_frame_queue[FRAME_QUEUE_SIZE];   /* 帧环形队列 */
static int g_q_write = 0;                           /* 写指针 */
static int g_q_read = 0;                            /* 读指针 */
static int g_q_count = 0;                           /* 队列中帧数 */
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_sender_thread;                   /* 发送线程句柄 */
static volatile bool g_sender_running = false;       /* 发送线程运行标志 */

static int g_q_dropped = 0;                         /* 队列满时丢弃的帧数 */

/*
 * 信号处理：SIGINT/SIGTERM 触发优雅退出
 */
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    }
}

/*
 * 检测 Annex-B 起始码 (00 00 00 01 或 00 00 01)
 */
static int find_start_code(unsigned char* data, int len, int pos) {
    if (pos + 3 >= len) return -1;
    if (data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 0 && data[pos+3] == 1) return 4;
    if (data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 1) return 3;
    return -1;
}

/*
 * Annex-B → AVCC 格式转换
 *
 * Annex-B:  00 00 00 01 [NAL data] 00 00 00 01 [NAL data] ...
 * AVCC:     [4B NALU长度(BE)] [NAL data] [4B NALU长度(BE)] [NAL data] ...
 */
static int annexb_to_avcc(unsigned char* src, int src_len, unsigned char* dst, int dst_size) {
    int dst_pos = 0;
    int i = 0;
    
    while (i < src_len) {
        int sc_len = find_start_code(src, src_len, i);
        if (sc_len < 0) break;
        
        i += sc_len;
        int nalu_start = i;
        
        int nalu_end = src_len;
        while (i < src_len - 3) {
            if (find_start_code(src, src_len, i) >= 0) {
                nalu_end = i;
                break;
            }
            i++;
        }
        
        int nalu_len = nalu_end - nalu_start;
        if (dst_pos + 4 + nalu_len > dst_size) return -1;
        
        dst[dst_pos++] = (nalu_len >> 24) & 0xFF;
        dst[dst_pos++] = (nalu_len >> 16) & 0xFF;
        dst[dst_pos++] = (nalu_len >> 8) & 0xFF;
        dst[dst_pos++] = nalu_len & 0xFF;
        
        memcpy(dst + dst_pos, src + nalu_start, nalu_len);
        dst_pos += nalu_len;
    }
    
    return dst_pos;
}

/*
 * 判断数据是否为 AVCC 格式
 */
static int is_avcc_format(unsigned char* data, int len) {
    if (len < 8) return 0;
    
    if (data[0] == 0 && data[1] == 0 && data[2] == 1) return 0;
    if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) return 0;
    
    int first_nalu_len = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    
    if (first_nalu_len > 0 && first_nalu_len < len && first_nalu_len < 100000) {
        unsigned char nalu_type = data[4] & 0x1F;
        if (nalu_type >= 1 && nalu_type <= 23) {
            return 1;
        }
    }
    
    return 0;
}

/*
 * 编码器回调函数 — 生产者端
 *
 * 由 GC4653 硬件编码器在每一帧编码完成后调用(编码器线程上下文)。
 * 这里只做最小工作：统计 + memcpy 入队 + 信号通知消费线程。
 */
void on_video_frame(unsigned char* data, int len,
                   unsigned long long timestamp, int is_keyframe) {
    g_frame_count++;
    g_total_bytes += len;
    if (is_keyframe) g_i_frame_count++;
    else g_p_frame_count++;
    
    if (g_first_hw_ts == 0) {
        g_first_hw_ts = timestamp;
        gettimeofday(&g_start_time, NULL);
    }
    
    unsigned long long hw_ts_ms = timestamp - g_first_hw_ts;
    
    if (g_conn == NULL || data == NULL || len <= 0) return;
    if (len > MAX_FRAME_SIZE) return;
    
    /* 入队: 将帧拷贝到环形队列，唤醒发送线程 */
    pthread_mutex_lock(&g_q_lock);
    if (g_q_count < FRAME_QUEUE_SIZE) {
        FrameItem& item = g_frame_queue[g_q_write];
        memcpy(item.data, data, len);
        item.len = len;
        /*
         * PTS 单位: 微秒（metaRTC 内部约定）
         *   Pacer 的 YangTimestamp 会执行 ×9/100 完成 µs→90kHz 转换，
         *   此处不可预先转换为 90kHz，否则二次转换导致时间轴压缩（幻灯片效果）
         */
        item.pts = (int64_t)hw_ts_ms * 1000;
        item.is_keyframe = is_keyframe;
        g_q_write = (g_q_write + 1) % FRAME_QUEUE_SIZE;
        g_q_count++;
        pthread_cond_signal(&g_q_cond);
    } else {
        g_q_dropped++;
    }
    pthread_mutex_unlock(&g_q_lock);
    
    if (g_frame_count % 100 == 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - g_start_time.tv_sec) * 1000 + 
                         (now.tv_usec - g_start_time.tv_usec) / 1000;
        double avg_fps = (double)g_frame_count / (elapsed_ms / 1000.0);
        double avg_kbps = (double)g_total_bytes * 8 / elapsed_ms;
        printf("[STATS] frames=%d (I=%d,P=%d) fps=%.1f kbps=%.1f  elapsed=%ldms  pts_ms=%llu  pts/wall=%.2f  q=%d\n\n",
               g_frame_count, g_i_frame_count, g_p_frame_count, avg_fps, avg_kbps,
               elapsed_ms, hw_ts_ms, elapsed_ms > 0 ? (double)hw_ts_ms / elapsed_ms : 0,
               g_q_count);
    }
}

/*
 * 发送线程 — 消费者端（metaRTC 8.0 版本）
 *
 * 与 7.0 版本的核心差异：
 *   1. Meta 提取使用 yang_meta_createH264 + yang_meta_getH264Flv
 *   2. NAL 解析使用 yang_nalu_getH264KeyframePos
 *   3. 推送使用 YangRtcPacer 将 YangFrame → YangPushData，再调用 on_video(YangPushData*)
 */
void* sender_thread_func(void* arg) {
    unsigned char avcc_buf[MAX_FRAME_SIZE];
    int sent_frame_count = 0;
    int sent_i_count = 0;
    int sent_p_count = 0;
    
    /* 8.0 新增：YangFrame 和 YangPushData 用于 Pacer 转换 */
    YangFrame videoFrame;
    YangPushData videoData;
    memset(&videoFrame, 0, sizeof(YangFrame));
    memset(&videoData, 0, sizeof(YangPushData));
    
    printf("[SEND-THREAD] started (8.0 API, NAL-stripped + meta push), waiting...\n");
    fflush(stdout);
    
    while (g_sender_running) {
        pthread_mutex_lock(&g_q_lock);
        while (g_q_count == 0 && g_sender_running) {
            pthread_cond_wait(&g_q_cond, &g_q_lock);
        }
        if (!g_sender_running) {
            pthread_mutex_unlock(&g_q_lock);
            break;
        }
        
        FrameItem item = g_frame_queue[g_q_read];
        g_q_read = (g_q_read + 1) % FRAME_QUEUE_SIZE;
        g_q_count--;
        pthread_mutex_unlock(&g_q_lock);
        
        sent_frame_count++;
        const char* type_str = item.is_keyframe ? "I" : "P";
        
        /* ---- 第1步: Annex-B → AVCC 格式转换 ---- */
        int avcc_len = item.len;
        unsigned char* frame_data = item.data;
        
        if (!is_avcc_format(item.data, item.len)) {
            avcc_len = annexb_to_avcc(item.data, item.len, avcc_buf, sizeof(avcc_buf));
            if (avcc_len > 0) {
                frame_data = avcc_buf;
            } else {
                continue;
            }
        }
        
        /* ---- 第2步: 构建 YangFrame ---- */
        memset(&videoFrame, 0, sizeof(YangFrame));
        videoFrame.payload = frame_data;
        videoFrame.nb = avcc_len;
        videoFrame.pts = item.pts;
        videoFrame.frametype = item.is_keyframe ? YANG_Frametype_I : YANG_Frametype_P;
        videoFrame.uid = 0;
        
        int ret = 0;
        
        if (item.is_keyframe) {
            /*
             * ---- 第3步(I帧): 提取并推送 SPS/PPS Meta（8.0 新 API）----
             *
             * 8.0 使用 yang_meta_createH264 从 AVCC 帧中提取 SPS/PPS，
             * yang_meta_getH264Flv 生成 AVCDecoderConfigurationRecord。
             */
            if (!g_meta_extracted) {
                memset(&g_h264Conf, 0, sizeof(YangH2645Conf));
                ret = yang_meta_createH264(&g_h264Conf, frame_data, avcc_len);
                if (ret == 0) {
                    g_meta_extracted = 1;
                }
            }
            
            if (g_meta_extracted && g_h264Conf.spsLen > 0) {
                /* 生成 FLV 格式的 AVCDecoderConfigurationRecord */
                unsigned char meta_buf[128];
                int32_t meta_len = 0;
                yang_meta_getH264Flv(&g_h264Conf, meta_buf, &meta_len);
                
                if (meta_len > 0) {
                    YangFrame metaFrame;
                    memset(&metaFrame, 0, sizeof(YangFrame));
                    metaFrame.payload = meta_buf;
                    metaFrame.nb = meta_len;
                    metaFrame.pts = videoFrame.pts;
                    metaFrame.frametype = YANG_Frametype_Spspps;
                    metaFrame.uid = 0;
                    
                    /* 8.0: Meta 也通过 Pacer 转换后推送 */
                    YangPushData* metaData = g_pacer->getVideoData(&metaFrame);
                    if (metaData) {
                        g_conn->on_video(metaData);
                    }
                    
                    if (sent_frame_count <= 3) {
                        printf("[SEND-META] meta_len=%d (SPS=%d PPS=%d)\n",
                               meta_len, g_h264Conf.spsLen, g_h264Conf.ppsLen);
                        fflush(stdout);
                    }
                }
            }
            
            /*
             * ---- 第4步(I帧): NAL 解析 → 提取纯 IDR NAL（8.0 新 API）----
             *
             * yang_nalu_getH264KeyframePos 返回 AVCC 数据中 IDR NAL 的位置
             */
            int32_t keyframePos = yang_nalu_getH264KeyframePos(frame_data, avcc_len);
            
            if (keyframePos > 0 && keyframePos + 4 < avcc_len) {
                videoFrame.payload = frame_data + keyframePos + 4;
                videoFrame.nb = avcc_len - keyframePos - 4;
            } else {
                /* 解析失败时回退: 仅跳过 AVCC 前缀 */
                videoFrame.payload = frame_data + 4;
                videoFrame.nb = avcc_len - 4;
            }
        } else {
            /* P帧: 剥离 AVCC 4-byte 长度前缀 */
            videoFrame.payload = frame_data + 4;
            videoFrame.nb = avcc_len - 4;
        }
        
        /* ---- 第5步: 通过 Pacer 转换并推送裸 NAL 到 MetaRTC 8.0 ---- */
        YangPushData* pushData = g_pacer->getVideoData(&videoFrame);
        if (pushData) {
            ret = g_conn->on_video(pushData);
        }
        
        /* 选择性日志 */
        int should_log = (sent_frame_count <= 10) || 
                        (item.is_keyframe) || 
                        (sent_frame_count % 50 == 0);
        
        if (should_log) {
            printf("[SEND] #%d %s orig=%d/%d nal=%d pts=%lld ret=%d hex=%02x%02x%02x%02x\n",
                   sent_frame_count, type_str, item.len, avcc_len, videoFrame.nb,
                   (long long)item.pts, ret,
                   ((unsigned char*)videoFrame.payload)[0],
                   ((unsigned char*)videoFrame.payload)[1],
                   ((unsigned char*)videoFrame.payload)[2],
                   ((unsigned char*)videoFrame.payload)[3]);
            fflush(stdout);
        }
        
        if (item.is_keyframe) sent_i_count++;
        else sent_p_count++;
        
        if (sent_frame_count % 100 == 0) {
            printf("[SEND-STATS] total=%d I=%d P=%d\n",
                   sent_frame_count, sent_i_count, sent_p_count);
            fflush(stdout);
        }
    }
    
    printf("[SEND-THREAD] stopped: total_sent=%d I=%d P=%d\n",
           sent_frame_count, sent_i_count, sent_p_count);
    fflush(stdout);
    return NULL;
}

/*
 * 主函数（metaRTC 8.0 版本）
 *
 * 启动流程与 7.0 相同，但新增 YangRtcPacer 初始化步骤
 */
int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    const char* whip_url = "http://192.168.137.10:1985/rtc/v1/whip/?app=live&stream=test3";
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            whip_url = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("用法: %s [-u <whip_url>]\n", argv[0]);
            printf("  -u  WHIP 推流地址\n");
            return 0;
        }
    }
    
    printf("========================================\n");
    printf("  WebRTC 推流 (metaRTC 8.0)\n");
    printf("========================================\n\n");
    printf("推流地址: %s\n", whip_url);
    printf("视频参数: 640x480 @ 20fps, 2048kbps\n\n");
    
    /* ===== 阶段1: 初始化 YangAVInfo ===== */
    YangAVInfo avinfo;
    memset(&avinfo, 0, sizeof(YangAVInfo));
    avinfo.sys.mediaServer = Yang_Server_Whip_Whep;
    avinfo.audio.sample = 48000;
    avinfo.audio.channel = 2;
    avinfo.audio.audioEncoderType = Yang_AED_OPUS;
    avinfo.audio.enableAudioFec = yangfalse;
    avinfo.video.width = 640;
    avinfo.video.height = 480;
    avinfo.video.outWidth = 640;
    avinfo.video.outHeight = 480;
    avinfo.video.frame = 20;
    avinfo.video.videoCacheNum = 10;
    avinfo.video.evideoCacheNum = 10;
    avinfo.video.videoPlayCacheNum = 10;
    avinfo.video.videoEncoderType = Yang_VED_H264;
    avinfo.rtc.rtcLocalPort = 17000;
    avinfo.rtc.iceCandidateType = YangIceHost;
    avinfo.enc.enc_threads = 4;
    
    /* ===== 阶段2: 创建 PeerConnection8 ===== */
    YangPeerInfo peerInfo;
    yang_avinfo_initPeerInfo(&peerInfo, &avinfo);
    peerInfo.direction = YangSendonly;
    
    g_conn = new YangPeerConnection8(&peerInfo, NULL, NULL, NULL, NULL);
    g_conn->addAudioTrack(Yang_AED_OPUS);
    g_conn->addVideoTrack(Yang_VED_H264);
    g_conn->addTransceiver(YangMediaAudio, peerInfo.direction);
    g_conn->addTransceiver(YangMediaVideo, peerInfo.direction);
    
    /* ===== 阶段2.5: 初始化 YangRtcPacer（8.0 新增）===== */
    g_pacer = new YangRtcPacer();
    g_pacer->initVideo(Yang_VED_H264, 1024);
    g_pacer->initAudio(Yang_AED_OPUS, 48000, 2);
    printf("YangRtcPacer 已初始化\n");
    
    /* ===== 阶段3: WHIP 连接 SRS ===== */
    int ret = yang_whip_connectWhipWhepServer(&g_conn->m_peer, (char*)whip_url);
    if (ret) {
        fprintf(stderr, "WHIP 连接失败: ret=%d\n", ret);
        return 1;
    }
    
    printf("WHIP 连接成功！\n\n");
    
    /* ===== 阶段4: 等待 WebRTC 底层就绪 ===== */
    printf("等待 SRTP + TCC 反馈回路 + jitter rebase 就绪 (6秒)...\n");
    sleep(6);
    
    /* ===== 阶段5: 启动发送线程 ===== */
    g_sender_running = true;
    pthread_create(&g_sender_thread, NULL, sender_thread_func, NULL);
    printf("发送线程已启动\n");
    
    /* ===== 阶段6: 启动 CameraDevice 视频采集 ===== */
    printf("正在初始化视频采集...\n");
    
    auto config = camera::CameraConfig::builder()
        .deviceId(0)
        .sensorConfig("/etc/isp_sensor.conf")
        .main(640, 480, 20, 2048)
        .build();
    
    g_cameraDevice.reset(new camera::CameraDevice(config));
    
    g_mainSubId = g_cameraDevice->subscribe(camera::ChannelId::Main,
        [](const camera::VideoFrame& frame) {
            on_video_frame(
                const_cast<unsigned char*>(frame.data()),
                frame.size(),
                frame.timestamp(),
                frame.isKeyFrame() ? 1 : 0);
        });
    
    camera::Error err = g_cameraDevice->start();
    if (err != camera::Error::Ok) {
        fprintf(stderr, "CameraDevice 启动失败: %d\n", (int)err);
        g_sender_running = false;
        pthread_cond_signal(&g_q_cond);
        pthread_join(g_sender_thread, NULL);
        return 1;
    }
    
    printf("CameraDevice 启动成功\n");
    printf("\n推流已启动！按 Ctrl+C 停止...\n\n");
    
    /* ===== 阶段7: 主循环 ===== */
    while (g_running) {
        usleep(100000);
    }
    
    /* ===== 阶段8: 优雅退出 ===== */
    printf("\n正在停止推流...\n");
    
    if (g_cameraDevice) {
        g_cameraDevice->stop();
        g_cameraDevice.reset();
        printf("CameraDevice 已停止\n");
    }
    
    g_sender_running = false;
    pthread_cond_signal(&g_q_cond);
    pthread_join(g_sender_thread, NULL);
    printf("发送线程已停止 (dropped=%d)\n", g_q_dropped);
    
    /* 释放 8.0 新增资源 */
    if (g_pacer) {
        delete g_pacer;
        g_pacer = NULL;
    }
    if (g_conn) {
        delete g_conn;
        g_conn = NULL;
    }
    
    printf("推流已停止\n");
    return 0;
}
