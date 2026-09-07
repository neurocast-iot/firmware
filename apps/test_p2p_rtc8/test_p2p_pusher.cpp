/**
 * test_p2p_pusher.cpp — P2P 推流测试程序（设备端，metaRTC 8.0）
 *
 * 架构：
 *   CameraDevice（H264 编码回调）→ P2PStreamer（answer 方连接层）
 *   P2PSignaling（MQTT 信令层）←→ viewer（offer 方，x86）
 *
 * 会话流程（事件驱动，支持多次会话）：
 *   1. 连接 MQTT broker，订阅 p2p/{devId}/offer 与 p2p/{devId}/cand/viewer
 *   2. 收到 offer → handleOffer 生成 answer → sendAnswer 回发
 *   3. onIceCandidate 本端候选 → sendCandidate 外发（Trickle）
 *   4. 收到对端 candidate → handleRemoteCandidate 注入
 *   5. waitConnected（ICE/DTLS/SRTP 就绪）→ startSending 开始推流
 *   6. 收到 bye 或 Ctrl+C → stopSession 优雅退出（幂等，可重新接收 offer）
 *
 * 用法：
 *   ./test_p2p_pusher -b <broker_url> -d <devId> [-n host|stun|turn]
 *     -b  MQTT broker 地址（必选，如 tcp://192.168.1.100:1883）
 *     -d  设备标识（必选，信令 topic 前缀 p2p/{devId}）
 *     -n  ICE 候选策略: host=仅内网直连(默认) stun=Srflx turn=Relay 中继
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <mutex>
#include <deque>
#include <atomic>
#include <netdb.h>
#include <arpa/inet.h>

#include "P2PSignaling.h"
#include "P2PStreamer.h"

#include "camera/camera_device.h"
#include "camera/camera_config.h"
#include "camera/types.h"
#include "camera/error.h"

static volatile sig_atomic_t g_running = 1;

/* CameraDevice 全局实例 */
static std::unique_ptr<camera::CameraDevice> g_cameraDevice;
static camera::SubscriptionId g_mainSubId = 0;

/* 当前会话 ID（信令回调线程与主线程共享，串话校验用） */
static std::mutex g_sidMutex;
static std::string g_currentSid;

/*
 * 信令消息队列：paho 回调线程仅入队，主循环消费。
 * 原因：paho 同步客户端的 messageArrived 与 PUBACK 处理共用同一接收线程，
 * 若在回调内 publish(QoS1)+waitForCompletion 会因 PUBACK 无法被处理而超时；
 * handleOffer（重建 PeerConnection）也不应阻塞接收线程。
 */
struct SigMsg {
    std::string type;
    std::string sid;
    std::string body;
};
static std::mutex g_msgMutex;
static std::deque<SigMsg> g_msgQueue;

/*
 * 信号处理: SIGINT/SIGTERM 触发优雅退出
 */
static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    }
}

/*
 * 域名解析（IPv4）：metaRTC 的 yang_addr_set 仅支持点分十进制 IP，
 * 不做 DNS 解析，因此 STUN/TURN 域名必须在应用层先解析为 IP 再传入。
 */
static bool resolveHostIPv4(const char* host, std::string& outIp) {
    struct addrinfo hints;
    struct addrinfo* res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) {
        return false;
    }
    char buf[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in* sin = (struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);

    outIp = buf;
    return !outIp.empty();
}

/*
 * 按 ICE 模式装配 P2PConfig（视频参数与 CameraDevice 主通道一致）
 */
static bool buildP2PConfig(const char* iceMode, P2PConfig& cfg) {
    cfg.width        = 1920;
    cfg.height       = 1080;
    cfg.fps          = 20;
    cfg.bitrate      = 4096;    /* kbps */
    cfg.rtcLocalPort = 17100;   /* 与 WHIP demo(17000) 错开 */

    if (strcmp(iceMode, "host") == 0) {
        cfg.iceCandidateType = 0;   /* 仅 Host 候选（局域网直连） */
        return true;
    }

    /* STUN/TURN 均使用自建 coturn（同一端口），域名启动时解析为 IP */
    const char* iceHost = "stun.example.com";
    std::string iceIp;
    if (!resolveHostIPv4(iceHost, iceIp)) {
        fprintf(stderr, "ICE 服务器域名解析失败: %s（请检查网络/DNS）\n", iceHost);
        return false;
    }
    printf("%s 服务器: %s -> %s:3478\n",
           strcmp(iceMode, "turn") == 0 ? "TURN" : "STUN", iceHost, iceIp.c_str());

    cfg.iceServerIP   = iceIp;
    cfg.iceServerPort = 3478;
    if (strcmp(iceMode, "turn") == 0) {
        cfg.iceCandidateType = 2;   /* Relay 中继 */
        /* TURN Allocate 走长期凭证认证（coturn lt-cred-mech） */
        cfg.iceUserName = "turn_user";
        cfg.icePassword = "turn_password";
    } else {
        cfg.iceCandidateType = 1;   /* Srflx（STUN Binding 免认证） */
    }
    return true;
}

/*
 * 主函数
 */
int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const char* broker_url = nullptr;   /* MQTT broker 地址，必选 */
    const char* dev_id     = nullptr;   /* 设备标识，必选 */
    const char* ice_mode   = "host";    /* ICE 候选策略: host(默认)/stun/turn */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            broker_url = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dev_id = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            ice_mode = argv[++i];
            if (strcmp(ice_mode, "host") != 0 && strcmp(ice_mode, "stun") != 0
                    && strcmp(ice_mode, "turn") != 0) {
                fprintf(stderr, "无效 ICE 模式: %s（仅支持 host/stun/turn）\n", ice_mode);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("用法: %s -b <broker_url> -d <devId> [-n host|stun|turn]\n", argv[0]);
            printf("  -b  MQTT broker 地址（必选，如 tcp://192.168.1.100:1883）\n");
            printf("  -d  设备标识（必选，信令 topic 前缀 p2p/{devId}）\n");
            printf("  -n  ICE 候选策略: host=内网直连(默认) stun=Srflx turn=Relay\n");
            return 0;
        }
    }

    if (broker_url == nullptr || dev_id == nullptr) {
        fprintf(stderr, "缺少必选参数\n用法: %s -b <broker_url> -d <devId> [-n host|stun|turn]\n",
                argv[0]);
        return 1;
    }

    printf("========================================\n");
    printf("  P2P 推流测试 (metaRTC 8.0, answer 方)\n");
    printf("========================================\n\n");
    printf("MQTT broker: %s\n", broker_url);
    printf("设备标识:    %s\n", dev_id);
    printf("ICE 模式:    %s\n", ice_mode);
    printf("视频参数:    1920x1080 @ 20fps, 4096kbps\n\n");

    /* ===== 阶段1: 初始化 P2PStreamer（堆分配，帧队列 8MB 避免栈溢出） ===== */
    P2PConfig cfg;
    if (!buildP2PConfig(ice_mode, cfg)) {
        return 1;
    }

    std::unique_ptr<P2PStreamer> streamer(new P2PStreamer());
    if (!streamer->init(cfg)) {
        fprintf(stderr, "P2PStreamer 初始化失败\n");
        return 1;
    }

    /* ===== 阶段2: 连接信令通道（MQTT） ===== */
    P2PSignaling signaling(broker_url, dev_id, "pusher");

    /* 本端 ICE 候选外发：gather 线程触发，P2PSignaling 内部有锁保护 */
    streamer->setCandidateHandler([&signaling](const std::string& candidate) {
        std::lock_guard<std::mutex> lock(g_sidMutex);
        signaling.sendCandidate(g_currentSid, candidate);
    });

    /* 信令消息入队（paho 接收线程上下文，仅拷贝，不阻塞） */
    signaling.setMessageHandler([](const std::string& type, const std::string& sid,
                                   const std::string& body) {
        std::lock_guard<std::mutex> lock(g_msgMutex);
        g_msgQueue.push_back(SigMsg{type, sid, body});
    });

    if (!signaling.connect()) {
        fprintf(stderr, "MQTT 信令连接失败: %s\n", broker_url);
        return 1;
    }

    /* ===== 阶段3: 启动 CameraDevice 视频采集（帧持续入队，无会话时被回调丢弃） ===== */
    printf("正在初始化视频采集...\n");

    auto camCfg = camera::CameraConfig::builder()
        .deviceId(0)
        .sensorConfig("/etc/isp_sensor.conf")
        .main(1920, 1080, 20, 4096)   /* 需与 buildP2PConfig 推流参数一致 */
        .build();

    g_cameraDevice.reset(new camera::CameraDevice(camCfg));

    /* 订阅主通道帧，转发给 P2PStreamer（无活动会话时 frameCallback 内部直接丢弃） */
    g_mainSubId = g_cameraDevice->subscribe(camera::ChannelId::Main,
        [](const camera::VideoFrame& frame) {
            P2PStreamer::frameCallback(
                const_cast<unsigned char*>(frame.data()),
                frame.size(),
                frame.timestamp(),
                frame.isKeyFrame() ? 1 : 0);
        });

    camera::Error err = g_cameraDevice->start();
    if (err != camera::Error::Ok) {
        fprintf(stderr, "CameraDevice 启动失败: %d\n", (int)err);
        g_cameraDevice.reset();
        return 1;
    }

    printf("CameraDevice 启动成功\n");
    printf("\n等待 viewer 发起 offer（topic: p2p/%s/offer）... 按 Ctrl+C 退出\n\n", dev_id);

    /* ===== 阶段4: 主循环（消费信令队列 + 协商完成后启动推流） ===== */
    bool negotiating = false;   /* offer 处理成功后置位，进入连接等待 */
    int  waitedMs = 0;          /* 连接等待计时（上限 30s） */

    while (g_running) {
        /* --- 消费信令消息（逐条处理，保持 offer→candidate 顺序） --- */
        for (;;) {
            SigMsg msg;
            {
                std::lock_guard<std::mutex> lock(g_msgMutex);
                if (g_msgQueue.empty()) break;
                msg = g_msgQueue.front();
                g_msgQueue.pop_front();
            }

            if (msg.type == "offer") {
                printf("[main] 收到 offer (sid=%s, %zu bytes)\n",
                       msg.sid.c_str(), msg.body.size());
                {
                    std::lock_guard<std::mutex> lock(g_sidMutex);
                    g_currentSid = msg.sid;   /* 新会话覆盖旧会话（handleOffer 内部幂等复位） */
                }
                std::string answerSdp;
                if (!streamer->handleOffer(msg.body, answerSdp)) {
                    fprintf(stderr, "[main] handleOffer 失败\n");
                    negotiating = false;
                    continue;
                }
                if (!signaling.sendAnswer(msg.sid, answerSdp)) {
                    fprintf(stderr, "[main] answer 回发失败\n");
                    streamer->stopSession();
                    negotiating = false;
                    continue;
                }
                negotiating = true;
                waitedMs = 0;
            } else if (msg.type == "candidate") {
                /* sid 校验：拒绝串话（旧会话残留候选） */
                bool sidMatch;
                {
                    std::lock_guard<std::mutex> lock(g_sidMutex);
                    sidMatch = (msg.sid == g_currentSid);
                }
                if (!sidMatch) {
                    printf("[main] candidate sid 不匹配，丢弃 (sid=%s)\n", msg.sid.c_str());
                    continue;
                }
                streamer->handleRemoteCandidate(msg.body);
            } else if (msg.type == "bye") {
                bool sidMatch;
                {
                    std::lock_guard<std::mutex> lock(g_sidMutex);
                    sidMatch = (msg.sid == g_currentSid);
                }
                if (sidMatch) {
                    printf("[main] 收到 bye (sid=%s)，结束会话\n", msg.sid.c_str());
                    negotiating = false;
                    streamer->stopSession();
                }
            }
        }

        /* --- 兜底终止：对端 DTLS close_notify（信令 bye 丢失/页面崩溃等场景） --- */
        if (streamer->isSessionActive() && streamer->isPeerClosed()) {
            printf("[main] 对端 DTLS 已关闭（close_notify），结束会话\n");
            negotiating = false;
            streamer->stopSession();
        }

        /* --- 连接等待：非阻塞轮询，不影响后续 candidate 注入 --- */
        if (negotiating) {
            if (streamer->isConnected()) {
                streamer->startSending();
                printf("[main] P2P 连接建立，推流中...\n");
                negotiating = false;
            } else {
                waitedMs += 100;
                if (waitedMs >= 30000) {
                    fprintf(stderr, "[main] P2P 连接超时(30s)，会话终止\n");
                    streamer->stopSession();
                    negotiating = false;
                }
            }
        }

        usleep(100000);
    }

    /* ===== 阶段5: 优雅退出 ===== */
    printf("\n正在停止推流...\n");

    if (g_cameraDevice) {
        /* 先取消订阅再停设备，避免停止过程中回调打到已停的 streamer */
        g_cameraDevice->unsubscribe(g_mainSubId);
        g_cameraDevice->stop();
        g_cameraDevice.reset();
    }

    /* 通知对端会话结束（尽力而为），再断开信令 */
    {
        std::lock_guard<std::mutex> lock(g_sidMutex);
        if (!g_currentSid.empty() && streamer->isSessionActive()) {
            signaling.sendBye(g_currentSid);
        }
    }
    streamer->stopSession();
    signaling.disconnect();

    printf("推流已停止\n");
    return 0;
}
