/**
 * inproc_queue.h
 * 进程内消息队列
 *
 * 使用 ZeroMQ 的 PUB/SUB 模式实现同一进程内部不同模块间的消息传递。
 * PUBLISHER socket 用于发送，SUBSCRIBER socket 用于接收。
 * 使用全局共享的 ZeroMQ Context（ZmqContextManager）。
 *
 * 特点：
 * - 快速：不经过网络协议栈，直接在内存中传递
 * - 低延迟：通常 < 10 微秒
 * - 线程安全：ZeroMQ context 内部线程安全
 *
 * 使用示例：
 *   InprocQueue queue;
 *   queue.initialize("inproc://main_queue");
 *   queue.start();
 *   queue.subscribe(InprocMessageType::PHOTO_CAPTURED, callback);
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
 * 进程内消息队列类
 *
 * 封装 ZeroMQ inproc 传输，提供消息发布/订阅接口。
 * 内部使用独立接收线程，消息到达时通过回调函数通知订阅者。
 */
class InprocQueue {
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
     * 初始化运行标志和初始化标志为 false。
     * 需要调用 initialize() 和 start() 后才能使用。
     */
    InprocQueue();

    /**
     * 析构函数
     *
     * 自动调用 stop() 停止接收线程并释放 ZeroMQ 资源。
     */
    ~InprocQueue();

    /**
     * 初始化进程内消息队列
     *
     * 从全局 Context 创建 PUBLISHER 和 SUBSCRIBER socket。
     * PUBLISHER 绑定到指定端点，SUBSCRIBER 连接到同一端点并订阅所有消息。
     *
     * @param endpoint 端点地址，格式为 "inproc://队列名称"
     * @param config 配置参数（可选），包含超时、高水位、日志回调等
     * @return true=初始化成功, false=端点占用或 ZeroMQ 错误
     */
    bool initialize(const std::string& endpoint, const MqConfig& config = MqConfig{});

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
     * 设置运行标志为 false，等待接收线程退出，然后关闭 socket。
     * 析构函数会自动调用此方法。
     */
    void stop();

    /**
     * 发送消息
     *
     * 将消息头和消息负载通过 PUBLISHER socket 发送。
     * 消息格式：[MessageHeader 二进制][payload JSON 字符串]
     *
     * @param header 消息头，包含消息类型、时间戳等信息
     * @param payload 消息负载（JSON 格式字符串），可为空
     * @return MqResult 操作结果
     */
    MqResult send(const MessageHeader& header, const std::string& payload);

    /**
     * 订阅指定类型的消息（库内置类型）
     *
     * 注册回调函数，当收到指定类型的消息时自动调用回调。
     * 同一消息类型只能注册一个回调，重复订阅会覆盖之前的回调。
     *
     * @param type 要订阅的消息类型
     * @param callback 消息到达时的回调函数
     */
    void subscribe(InprocMessageType type, MessageCallback callback);

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
    void unsubscribe(InprocMessageType type);

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

    /** 检查队列是否已初始化 */
    bool isInitialized() const { return initialized_; }

    /** 检查队列是否正在运行 */
    bool isRunning() const { return running_; }

private:
    /** 输出日志 */
    void log(LogLevel level, const std::string& msg);

    /**
     * 接收循环（运行在独立线程中）
     *
     * 持续从 SUBSCRIBER socket 接收消息，解析后分发到对应回调。
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

    std::string endpoint_;                    // 端点地址（如 "inproc://main_queue"）
    zmq::socket_t* publisher_ = nullptr;      // PUBLISHER socket，用于发送消息
    zmq::socket_t* subscriber_ = nullptr;     // SUBSCRIBER socket，用于接收消息

    std::atomic<bool> running_{false};        // 运行标志，控制接收线程生命周期
    std::atomic<bool> initialized_{false};    // 初始化标志
    std::unique_ptr<std::thread> receiveThread_;  // 接收线程

    std::mutex callbackMutex_;                // 回调函数表的互斥锁
    std::map<uint32_t, MessageCallback> callbacks_;  // 消息类型到回调的映射（使用 uint32_t 支持自定义类型）

    MqConfig::LogCallback logCallback_;       // 外部日志回调函数（包含日志级别参数）
    int recvTimeoutMs_ = 1000;                // 接收超时（毫秒）
};

}
