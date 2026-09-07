/**
 * config.h
 * lib-mq 配置管理
 *
 * 定义消息队列的配置结构体，包含端点、超时、高水位等参数。
 * 所有配置项都有合理默认值，调用方只需设置必要的端点即可。
 */
#pragma once
#include <string>
#include <functional>
#include "libmq/result.h"

namespace libmq {

/**
 * IPC 通信模式
 *
 * 定义进程间通信的工作模式：
 * - PairServer: PAIR 服务端（bind 端点，一对一）
 * - PairClient: PAIR 客户端（connect 到服务端，一对一）
 * - Publisher: PUB 发布端（bind 端点，一对多广播）
 * - Subscriber: SUB 订阅端（connect 到发布端，接收消息）
 * - Router: ROUTER 服务端（bind 端点，多对多，中心节点用）
 * - Dealer: DEALER 客户端（connect 到 Router，多对多）
 */
enum class IpcMode {
    PairServer,     ///< PAIR 服务端（bind 端点，一对一双向通信）
    PairClient,     ///< PAIR 客户端（connect 到服务端，一对一双向通信）
    Publisher,      ///< PUB 发布端（bind 端点，一对多单向广播）
    Subscriber,     ///< SUB 订阅端（connect 到发布端，接收消息）
    Router,         ///< ROUTER 服务端（bind 端点，多对多双向，中心节点用）
    Dealer          ///< DEALER 客户端（connect 到 Router，多对多双向）
};

/**
 * 消息队列配置
 *
 * 通过 Builder 链式设置，也可直接构造后传入。
 * 配置项按用途分为：端点配置、超时配置、高水位配置、日志配置。
 */
struct MqConfig {
    // ========== 端点配置（必须设置） ==========
    std::string inprocEndpoint;   // 进程内端点，如 "inproc://main"
    std::string ipcEndpoint;      // 进程间端点，如 "ipc:///tmp/iot_agent.ipc"；为空则不启用 IPC
    IpcMode ipcMode = IpcMode::PairServer;  ///< IPC 通信模式（默认 PAIR 服务端）

    // ========== 身份标识（Router/Dealer 模式用） ==========
    std::string peerName;         // 本节点名字，如 "iot_agent"、"ota_agent"
                                  // Router 模式：用于标识自己
                                  // Dealer 模式：用于发消息时填 from 字段

    // ========== 超时配置 ==========
    int sendTimeoutMs = 100;      // 发送超时（毫秒），防止发送线程无限阻塞
    int recvTimeoutMs = 1000;     // 接收超时（毫秒），接收线程循环间隔

    // ========== 高水位配置 ==========
    int sendHwm = 1000;           // 发送高水位（消息数），超过后丢弃旧消息
    int recvHwm = 1000;           // 接收高水位（消息数）

    // ========== 日志配置 ==========
    /**
     * 日志回调函数类型
     *
     * 调用方可在此函数中将日志输出到 spdlog/syslog/控制台等。
     *
     * @param level 日志级别（Debug/Info/Warn/Error）
     * @param msg 日志消息内容（不包含级别前缀）
     */
    using LogCallback = std::function<void(LogLevel level, const std::string& msg)>;
    LogCallback logCallback;      // 日志回调，不设置则丢弃内部日志

    /**
     * 检查配置是否合法
     * 
     * @return true=配置有效, false=配置无效（缺少必要的 inprocEndpoint）
     */
    bool isValid() const {
        return !inprocEndpoint.empty();
    }
};

}
