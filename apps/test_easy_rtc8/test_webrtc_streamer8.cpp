/**
 * test_webrtc_streamer8.cpp — WebRTCStreamer 类集成测试（metaRTC 8.0 版本）
 *
 * 功能：
 *   - 验证 WebRTCStreamer 类的完整功能（8.0 API）
 *   - 使用帧回调模式 (模拟 MainChannelDispatcher 分发)
 *   - CameraDevice 编码器回调 → WebRTCStreamer::frameCallback → 推流
 *
 * 与 main.cpp 的区别：
 *   - main.cpp: 直接使用 YangPeerConnection8 + 自建队列 (低层级)
 *   - 本文件: 通过 WebRTCStreamer 封装 (对标 RTMPStreamer 模式)
 *
 * 用法：
 *   ./test_webrtc_streamer8 -u <whip_url> [-n host|stun|turn]
 *     -u       WHIP 推流地址（必选）
 *     -n host  Host 直连模式（默认）：局域网/公网 SRS 通用（SRS 为 ICE-Lite）
 *     -n stun  STUN 模式演示：ICE Srflx（仅对 Full-ICE 对端生效）
 *     -n turn  TURN 模式演示：ICE Relay 中继（仅对 Full-ICE 对端生效）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <netdb.h>
#include <arpa/inet.h>

#include "WebRTCStreamer.h"

#include "camera/camera_device.h"
#include "camera/camera_config.h"
#include "camera/types.h"
#include "camera/error.h"

static volatile sig_atomic_t g_running = 1;

/* CameraDevice 全局实例 */
static std::unique_ptr<camera::CameraDevice> g_cameraDevice;
static camera::SubscriptionId g_mainSubId = 0;

/*
 * 信号处理: SIGINT/SIGTERM 触发优雅退出
 */
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    }
}

/*
 * 域名解析（IPv4）：metaRTC 的 yang_addr_set 仅支持点分十进制 IP，
 * 不做 DNS 解析，因此 STUN 域名必须在应用层先解析为 IP 再传入。
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
 * Host 直连推流（局域网/公网 SRS 通用）
 *
 * SRS 为 ICE-Lite 实现：应答 SDP 自带服务器公网候选，且回包使用实际
 * 收包源地址（NAT 映射地址），metaRTC 收到 ICE-Lite 应答后会跳过整个
 * ICE 采集阶段（YangIce.c yang_ice_startIceAgent 提前返回），因此对 SRS
 * 推流无需 STUN/TURN，公网 4G 场景同样适用本模式。
 */
static bool startHostPush(WebRTCStreamer* webrtc, const char* whipUrl) {
    WebRTCConfig cfg;
    cfg.enabled      = true;
    cfg.whipUrl      = whipUrl;
    cfg.width        = 1920;
    cfg.height       = 1080;
    cfg.fps          = 20;
    cfg.bitrate      = 4096;   /* kbps */
    cfg.rtcLocalPort = 17000;
    /* iceCandidateType 保持默认 0=Host：SDP offer 只携带内网地址候选 */

    if (!webrtc->init(cfg)) {
        fprintf(stderr, "WebRTCStreamer 初始化失败\n");
        return false;
    }
    return webrtc->startPush(cfg.whipUrl);
}

/*
 * STUN/TURN 模式推流演示（useTurn: false=Stun srflx, true=Turn 中继）
 *
 * 注意：STUN/TURN 采集均在 metaRTC 的 gather 线程中执行，而对端为
 * ICE-Lite（如 SRS）且应答携带候选时该线程被跳过（YangIce.c L272），
 * 因此本模式仅在对接 Full-ICE 对端（如 metaRTC P2P）时实际生效；
 * 对 SRS 推流时行为退化为 Host 直连。
 */
static bool startIceServerPush(WebRTCStreamer* webrtc, const char* whipUrl, bool useTurn) {
    /* ICE 服务器（自建 coturn，STUN/TURN 同一端口，域名启动时解析为 IP） */
    const char* iceHost = "stun.example.com";
    const int   icePort = 3478;

    /* 域名解析失败直接判定失败（无服务器地址时 Stun/Turn 模式必不可达） */
    std::string iceIp;
    if (!resolveHostIPv4(iceHost, iceIp)) {
        fprintf(stderr, "ICE 服务器域名解析失败: %s（请检查网络/DNS）\n", iceHost);
        return false;
    }
    printf("%s 服务器: %s -> %s:%d\n", useTurn ? "TURN" : "STUN", iceHost, iceIp.c_str(), icePort);

    WebRTCConfig cfg;
    cfg.enabled      = true;
    cfg.whipUrl      = whipUrl;
    cfg.width        = 1920;
    cfg.height       = 1080;
    cfg.fps          = 20;
    cfg.bitrate      = 4096;   /* kbps */
    cfg.rtcLocalPort = 17000;
    cfg.iceCandidateType = useTurn ? 2 : 1;   /* 1=Stun(Srflx) 2=Turn(Relay) */
    cfg.iceServerIP      = iceIp;
    cfg.iceServerPort    = icePort;

    if (useTurn) {
        /* TURN Allocate 走长期凭证认证（coturn lt-cred-mech）；
         * STUN Binding 协议免认证，Stun 模式无需填写 */
        cfg.iceUserName = "turn_user";
        cfg.icePassword = "turn_password";
    }

    if (!webrtc->init(cfg)) {
        fprintf(stderr, "WebRTCStreamer 初始化失败\n");
        return false;
    }
    return webrtc->startPush(cfg.whipUrl);
}

/*
 * 主函数
 */
int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const char* whip_url = nullptr;    /* WHIP 推流地址，必须由 -u 传入 */
    const char* ice_mode = "host";     /* ICE 模式: host(默认)/stun/turn */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            whip_url = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            ice_mode = argv[++i];
            if (strcmp(ice_mode, "host") != 0 && strcmp(ice_mode, "stun") != 0
                    && strcmp(ice_mode, "turn") != 0) {
                fprintf(stderr, "无效 ICE 模式: %s（仅支持 host/stun/turn）\n", ice_mode);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("用法: %s -u <whip_url> [-n host|stun|turn]\n", argv[0]);
            printf("  -u  WHIP 推流地址（必选）\n");
            printf("  -n  ICE 模式: host=直连(默认,SRS 局域网/公网通用),\n");
            printf("               stun/turn=STUN/TURN 演示(仅对 Full-ICE 对端生效)\n");
            return 0;
        }
    }

    if (whip_url == nullptr) {
        fprintf(stderr, "缺少 -u <whip_url> 参数\n用法: %s -u <whip_url> [-n host|stun|turn]\n", argv[0]);
        return 1;
    }
    
    printf("========================================\n");
    printf("  WebRTCStreamer 集成测试 (metaRTC 8.0)\n");
    printf("========================================\n\n");
    printf("ICE 模式: %s\n", ice_mode);
    printf("推流地址: %s\n", whip_url);
    printf("视频参数: 1920x1080 @ 20fps, 4096kbps\n");
    printf("架构模式: CameraDevice → WebRTCStreamer (对标 RTMPStreamer)\n\n");
    
    /* ===== 阶段1: 创建 WebRTCStreamer (堆分配, 避免 ~8MB 帧队列栈溢出; 析构自动 cleanup) ===== */
    std::unique_ptr<WebRTCStreamer> webrtc(new WebRTCStreamer());
    
    /* ===== 阶段2: 按 ICE 模式启动推流（各自独立函数，含 init + startPush） ===== */
    bool push_ok;
    if (strcmp(ice_mode, "stun") == 0) {
        push_ok = startIceServerPush(webrtc.get(), whip_url, false);
    } else if (strcmp(ice_mode, "turn") == 0) {
        push_ok = startIceServerPush(webrtc.get(), whip_url, true);
    } else {
        push_ok = startHostPush(webrtc.get(), whip_url);
    }
    if (!push_ok) {
        fprintf(stderr, "WebRTC WHIP 推流启动失败\n");
        return 1;
    }

    /* ===== 阶段3: 启动 CameraDevice 视频采集 ===== */
    printf("正在初始化视频采集...\n");

    auto config = camera::CameraConfig::builder()
        .deviceId(0)
        .sensorConfig("/etc/isp_sensor.conf")
        .main(1920, 1080, 20, 4096)   /* 需与 startHostPush/startIceServerPush 推流参数一致 */
        .build();

    g_cameraDevice.reset(new camera::CameraDevice(config));

    /* 订阅主通道帧，转发给 WebRTCStreamer */
    g_mainSubId = g_cameraDevice->subscribe(camera::ChannelId::Main,
        [](const camera::VideoFrame& frame) {
            WebRTCStreamer::frameCallback(
                const_cast<unsigned char*>(frame.data()),
                frame.size(),
                frame.timestamp(),
                frame.isKeyFrame() ? 1 : 0);
        });

    camera::Error err = g_cameraDevice->start();
    if (err != camera::Error::Ok) {
        fprintf(stderr, "CameraDevice 启动失败: %d\n", (int)err);
        g_cameraDevice.reset();
        webrtc->stopPush();
        return 1;
    }

    printf("CameraDevice 启动成功\n");
    printf("\n推流已启动！按 Ctrl+C 停止...\n\n");

    /* ===== 阶段4: 主循环 ===== */
    while (g_running) {
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

    webrtc->stopPush();

    printf("推流已停止\n");
    return 0;
}
