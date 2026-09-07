/**
 * ipc_queue.h
 * 进程间消息队列
 *
 * 使用 ZeroMQ 的 IPC 传输方式（Unix Domain Socket），实现跨进程消息传递。
 * 采用 PAIR 模式：一对一双向通信，适用于固定配对的两个进程。
 * 使用全局共享的 ZeroMQ Context（ZmqContextManager）。
 *
 * 特点：
 * - 本地通信：不经过网络协议栈，通过 Unix Domain Socket 传输
 * - 低延迟：通常 < 100 微秒
 * - 一对一：PAIR 模式只支持两个 socket 配对通信
 *
 * 使用示例：
 *   // 服务端（iot_live 端）
 *   IpcQueue server;
 *   server.initializeAsServer("ipc:///tmp/iot_live.ipc");
 *   server.start();
 *
 *   // 客户端（ota_agent 端）
 *   IpcQueue client;
 *   client.initializeAsClient("ipc:///tmp/iot_live.ipc");
 *   client.start();
 */
#pragma once
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <zmq.hpp>
#include "libmq/message_types.h"
#include "libmq/config.h"
#include "libmq/result.h"

namespace libmq {

/**
 * 进程间消息队列类
 *
 * 封装 ZeroMQ IPC 传输，提供跨进程消息发送/接收接口。
 * 支持服务端（bind）和客户端（connect）两种模式。
 * 内部使用独立接收线程，消息到达时通过回调函数通知订阅者。
 */
class IpcQueue {
public:
    /**
     * 消息回调函数类型
     * 
     * @param header 消息头，包含消息类型、时间戳、序列号等信息
     * @param payload 消息负载（JSON 格式字符串）
     */
    using MessageCallback = std::function<void(const MessageHeader&, const std::string&)>;

    /**
     * 构造函数
     *
     * 初始化服务端标志为 false，运行标志和初始化标志为 false。
     * 需要调用 initializeAsServer() 或 initializeAsClient() 后才能使用。
     */
    IpcQueue();

    /**
     * 析构函数
     *
     * 自动调用 stop() 停止接收线程并释放 ZeroMQ 资源。
     */
    ~IpcQueue();

    /**
     * 初始化进程间消息队列（服务端模式）
     *
     * 从全局 Context 创建 PAIR socket 并绑定到指定 IPC 端点。
     * 服务端负责创建 IPC socket 文件，客户端连接到该端点。
     *
     * @param endpoint IPC 端点地址，格式为 "ipc://socket文件路径"
     * @param config 配置参数（可选），包含超时、高水位、日志回调等
     * @return true=初始化成功, false=端点已被占用或权限不足
     */
    bool initializeAsServer(const std::string& endpoint, const MqConfig& config = MqConfig{});

    /**
     * 初始化进程间消息队列（客户端模式）
     *
     * 从全局 Context 创建 PAIR socket 并连接到服务端创建的 IPC 端点。
     * 客户端不创建 socket 文件，只负责连接。
     *
     * @param endpoint IPC 端点地址，需与服务端绑定的端点一致
     * @param config 配置参数（可选），包含超时、高水位、日志回调等
     * @return true=初始化成功, false=服务端未启动或端点不存在
     */
    bool initializeAsClient(const std::string& endpoint, const MqConfig& config = MqConfig{});

    /**
     * 初始化进程间消息队列（发布者模式）
     *
     * 使用 ZMQ_PUB socket，绑定到指定 IPC 端点。
     * 多个订阅者可以连接到同一个发布者。
     * 发布者只发送消息，不接收消息。
     *
     * @param endpoint IPC 端点地址，格式为 "ipc://socket文件路径"
     * @param config 配置参数（可选），包含超时、高水位、日志回调等
     * @return true=初始化成功, false=端点已被占用或权限不足
     */
    bool initializeAsPublisher(const std::string& endpoint, const MqConfig& config = MqConfig{});

    /**
     * 初始化进程间消息队列（订阅者模式）
     *
     * 使用 ZMQ_SUB socket，连接到发布者创建的 IPC 端点。
     * 订阅者只接收消息，不发送消息。
     * 默认订阅所有消息类型（空字符串订阅过滤）。
     *
     * @param endpoint IPC 端点地址，需与发布者绑定的端点一致
     * @param config 配置参数（可选），包含超时、高水位、日志回调等
     * @return true=初始化成功, false=发布者未启动或端点不存在
     */
    bool initializeAsSubscriber(const std::string& endpoint, const MqConfig& config = MqConfig{});

    /**
     * 初始化进程间消息队列（ROUTER 模式 - 中心节点）
     *
     * 使用 ZMQ_ROUTER socket，bind 到指定端点。
     * 支持多个 DEALER 客户端同时连接，实现多对多双向通信。
     * ROUTER 会自动记录每个连接的身份标识，用于消息路由。
     *
     * @param endpoint IPC 端点地址，格式为 "ipc://socket文件路径"
     * @param config 配置参数，peerName 为本节点名字
     * @return true=初始化成功, false=端点已被占用或权限不足
     */
    bool initializeAsRouter(const std::string& endpoint, const MqConfig& config = MqConfig{});

    /**
     * 初始化进程间消息队列（DEALER 模式 - 边缘节点）
     *
     * 使用 ZMQ_DEALER socket，connect 到 ROUTER 的 IPC 端点。
     * 多个 DEALER 可连接到同一个 ROUTER，实现多对多双向通信。
     * ZMQ 自动处理重连，ROUTER 未启动时不报错。
     *
     * @param endpoint IPC 端点地址，需与 ROUTER 绑定的端点一致
     * @param config 配置参数，peerName 为本节点名字（用于消息 from 字段）
     * @return true=初始化成功
     */
    bool initializeAsDealer(const std::string& endpoint, const MqConfig& config = MqConfig{});

    /**
     * 启动消息队列
     *
     * 启动接收线程，开始监听并分发消息。
     * 必须在 initialize() 成功后调用。
     *
     * @return MqResult 操作结果
     */
    MqResult start();

    /**
     * 停止消息队列
     *
     * 停止接收线程，关闭 PAIR socket，释放 ZeroMQ 资源。
     * 析构函数会自动调用此方法。
     */
    void stop();

    /**
     * 发送消息
     *
     * 将消息头和消息负载通过 PAIR socket 发送给对端进程。
     * 消息格式：[MessageHeader 二进制][payload JSON 字符串]，使用多帧消息。
     * 发送超时为配置的 sendTimeoutMs，超时则认为对端未就绪。
     *
     * @param header 消息头，包含消息类型、时间戳等信息
     * @param payload 消息负载（JSON 格式字符串），可为空
     * @return MqResult 操作结果
     */
    MqResult send(const MessageHeader& header, const std::string& payload);

    /**
     * 发送消息给指定目标（ROUTER 模式用）
     *
     * ROUTER 模式下，需要指定目标 peer 的身份标识才能发送。
     * 消息格式：[targetIdentity][empty][header][payload]
     *
     * @param targetIdentity 目标 peer 的 ZMQ 身份标识
     * @param header 消息头
     * @param payload 消息负载
     * @return MqResult 操作结果
     */
    MqResult sendTo(const std::string& targetIdentity, const MessageHeader& header, const std::string& payload);

    /**
     * 订阅指定类型的消息（库内置类型）
     *
     * 注册回调函数，当收到指定类型的消息时自动调用回调。
     * 同一消息类型只能注册一个回调，重复订阅会覆盖之前的回调。
     *
     * @param type 要订阅的消息类型
     * @param callback 消息到达时的回调函数
     */
    void subscribe(IpcMessageType type, MessageCallback callback);

    /**
     * 订阅指定类型的消息（通用接口，支持自定义类型）
     *
     * 允许应用订阅自定义消息类型（10000-65535 范围）。
     *
     * @param type 要订阅的消息类型（可以是库内置或应用自定义类型）
     * @param callback 消息到达时的回调函数
     */
    void subscribe(uint32_t type, MessageCallback callback);

    /**
     * 取消订阅指定类型的消息
     *
     * 移除之前注册的回调函数。
     *
     * @param type 要取消订阅的消息类型
     */
    void unsubscribe(IpcMessageType type);

    /**
     * 取消订阅指定类型的消息（通用接口，支持自定义类型）
     *
     * @param type 要取消订阅的消息类型
     */
    void unsubscribe(uint32_t type);

    /**
     * 取消所有订阅
     *
     * 清空所有已注册的回调函数。
     * 通常在程序退出时调用。
     */
    void clearAll();

    /** 检查当前是否为服务端模式 */
    bool isServer() const { return isServer_; }

    /** 检查队列是否已初始化 */
    bool isInitialized() const { return initialized_; }

    /** 检查队列是否正在运行 */
    bool isRunning() const { return running_; }

    /** 检查是否为 ROUTER 模式 */
    bool isRouter() const { return isRouterMode_; }

    /** 检查是否为 DEALER 模式 */
    bool isDealer() const { return isDealerMode_; }

    /** 获取本节点名字 */
    const std::string& peerName() const { return peerName_; }

    /**
     * 根据 peer 名字获取 ZMQ 身份标识（ROUTER 模式用）
     *
     * @param name peer 名字（如 "ota_agent"）
     * @return ZMQ 身份标识，未找到返回空字符串
     */
    std::string getPeerIdentity(const std::string& name) {
        std::lock_guard<std::mutex> lock(peerMutex_);
        auto it = peerIdentities_.find(name);
        return (it != peerIdentities_.end()) ? it->second : "";
    }

private:
    /**
     * 输出日志（统一接口）
     * 
     * @param level 日志级别
     * @param msg 日志消息
     */
    void log(LogLevel level, const std::string& msg);

    /**
     * 接收循环（运行在独立线程中）
     *
     * 持续从 PAIR socket 接收消息，解析后分发到对应回调。
     * 当 running_ 为 false 时退出循环。
     */
    void receiveLoop();

    /**
     * 分发消息到对应回调函数
     *
     * 根据消息头中的类型字段查找已注册的回调，找到后调用。
     * 如果没有注册该类型的回调，则丢弃消息。
     *
     * @param header 消息头
     * @param payload 消息负载（JSON 字符串）
     */
    void dispatchMessage(const MessageHeader& header, const std::string& payload);

    std::string endpoint_;                    // 端点地址（如 "ipc:///tmp/iot_agent.ipc"）
    std::string peerName_;                    // 本节点名字（如 "iot_agent"）
    bool isServer_ = false;                   // 是否为服务端模式（true=bind, false=connect）
    bool recvSupported_ = true;               // socket 是否支持接收（ZMQ_PUB 只发不收，
                                              // 对其调 recv 会立即报 ENOTSUP 导致忙循环）
    bool isRouterMode_ = false;               // 是否为 ROUTER 模式（需要处理身份帧）
    bool isDealerMode_ = false;               // 是否为 DEALER 模式
    zmq::socket_t* socket_ = nullptr;         // PAIR/ROUTER/DEALER socket，用于发送和接收

    // ROUTER 模式：记录 peer 名字 -> ZMQ 身份标识的映射
    std::mutex peerMutex_;
    std::map<std::string, std::string> peerIdentities_;  // peerName -> ZMQ identity

    std::atomic<bool> running_{false};        // 运行标志，控制接收线程生命周期
    std::atomic<bool> initialized_{false};    // 初始化标志
    std::unique_ptr<std::thread> receiveThread_;  // 接收线程

    std::mutex callbackMutex_;                // 回调函数表的互斥锁
    std::map<uint32_t, MessageCallback> callbacks_;  // 消息类型到回调的映射（使用 uint32_t 支持自定义类型）

    MqConfig::LogCallback logCallback_;       // 外部日志回调函数
    int recvTimeoutMs_ = 1000;                // 接收超时（毫秒）
    int sendTimeoutMs_ = 100;                 // 发送超时（毫秒）
};

}
