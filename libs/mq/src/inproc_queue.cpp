/**
 * inproc_queue.cpp
 * 进程内消息队列实现
 *
 * 使用 ZeroMQ 的 PUB/SUB 模式实现进程内消息传递。
 * 从全局 Context 创建 socket，所有队列共享同一个 zmq::context_t。
 */
#include "libmq/inproc_queue.h"
#include "libmq/zmq_context.h"
#include <cstring>

namespace libmq {

/**
 * 构造函数
 *
 * 使用 default，所有成员变量在头文件中已初始化为默认值。
 */
InprocQueue::InprocQueue() {
}

/**
 * 析构函数
 *
 * 自动调用 stop() 停止接收线程并释放 socket 资源。
 */
InprocQueue::~InprocQueue() {
    stop();
}

/**
 * 初始化进程内消息队列
 *
 * 从全局 ZmqContextManager 创建 PUBLISHER 和 SUBSCRIBER socket。
 * PUBLISHER 绑定端点，SUBSCRIBER 连接同一端点并订阅所有消息。
 * 同时配置接收超时和高水位参数。
 *
 * @param endpoint 进程内端点，如 "inproc://main"
 * @param config 配置参数
 * @return true=初始化成功, false=端点占用或 ZeroMQ 错误
 */
bool InprocQueue::initialize(const std::string& endpoint, const MqConfig& config) {
    try {
        endpoint_ = endpoint;
        logCallback_ = config.logCallback;
        recvTimeoutMs_ = config.recvTimeoutMs;

        // 从全局 Context 创建 socket（整个进程共享同一个 context）
        auto& ctx = ZmqContextManager::instance();

        publisher_ = new zmq::socket_t(ctx, ZMQ_PUB);
        subscriber_ = new zmq::socket_t(ctx, ZMQ_SUB);

        // PUB bind + SUB connect 是 inproc 的标准用法
        publisher_->bind(endpoint_);
        subscriber_->connect(endpoint_);

        // 订阅所有消息（空字符串表示不过滤）
        subscriber_->set(zmq::sockopt::subscribe, "");

        // 设置接收超时，避免接收线程无限阻塞
        subscriber_->set(zmq::sockopt::rcvtimeo, recvTimeoutMs_);

        // 配置高水位（可选），防止消息堆积导致内存耗尽
        if (config.sendHwm > 0) {
            publisher_->set(zmq::sockopt::sndhwm, config.sendHwm);
        }
        if (config.recvHwm > 0) {
            subscriber_->set(zmq::sockopt::rcvhwm, config.recvHwm);
        }

        initialized_ = true;
        log(LogLevel::Info, "InprocQueue initialized: " + endpoint_);
        return true;
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "InprocQueue initialization failed: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        log(LogLevel::Error, "InprocQueue initialization failed: " + std::string(e.what()));
        return false;
    }
}

/**
 * 启动消息队列
 *
 * 启动独立接收线程，开始监听 SUBSCRIBER socket 并分发消息。
 * 必须在 initialize() 成功后调用。
 *
 * @return MqResult 操作结果
 */
MqResult InprocQueue::start() {
    // 防止重复启动
    if (running_) {
        log(LogLevel::Warn, "InprocQueue already running");
        return MqResult::success();
    }

    // 检查前置条件：必须先初始化
    if (!initialized_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "InprocQueue not initialized");
    }

    running_ = true;

    // 启动接收线程，独立运行 receiveLoop()
    receiveThread_ = std::make_unique<std::thread>(&InprocQueue::receiveLoop, this);

    log(LogLevel::Info, "InprocQueue started");
    return MqResult::success();
}

/**
 * 停止消息队列
 *
 * 设置运行标志为 false 通知接收线程退出，
 * 等待线程 join，然后删除 socket 释放资源。
 */
void InprocQueue::stop() {
    // 如果已经停止，直接返回（避免重复操作）
    if (!running_) {
        return;
    }

    // 步骤 1：设置运行标志为 false，通知接收线程退出
    running_ = false;

    // 步骤 2：等待接收线程退出（超时由 rcvtimeo 保证，最多 recvTimeoutMs_ 毫秒）
    if (receiveThread_ && receiveThread_->joinable()) {
        receiveThread_->join();
    }

    // 步骤 3：释放 PUBLISHER 和 SUBSCRIBER socket
    if (publisher_) {
        delete publisher_;
        publisher_ = nullptr;
    }
    if (subscriber_) {
        delete subscriber_;
        subscriber_ = nullptr;
    }

    log(LogLevel::Info, "InprocQueue stopped");
}

/**
 * 发送消息
 *
 * 通过 PUBLISHER socket 发送两帧消息：
 * 第一帧：MessageHeader 二进制数据（带 sndmore 标志）
 * 第二帧：payload JSON 字符串
 *
 * @param header 消息头
 * @param payload 消息负载（JSON 字符串）
 * @return MqResult 操作结果
 */
MqResult InprocQueue::send(const MessageHeader& header, const std::string& payload) {
    // 检查前置条件：PUBLISHER 必须存在且队列正在运行
    if (!publisher_ || !running_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "InprocQueue not running");
    }

    try {
        // [DEBUG] 记录发送
        log(LogLevel::Info, "[SEND] type=" + std::to_string(header.type) + ", payload_len=" + std::to_string(payload.length()));

        // 步骤 1：序列化消息头为 zmq::message_t 并发送（sndmore 表示还有后续帧）
        zmq::message_t headerMsg(sizeof(MessageHeader));
        std::memcpy(headerMsg.data(), &header, sizeof(MessageHeader));

        auto result = publisher_->send(headerMsg, zmq::send_flags::sndmore);
        if (!result) {
            return MqResult::fail(MqErrorCode::SendFailed,
                "Failed to send header: type=" + std::to_string(header.type));
        }

        // [DEBUG] header 发送成功
        log(LogLevel::Info, "[SEND] header sent OK, type=" + std::to_string(header.type));

        // 步骤 2：序列化 payload 为 zmq::message_t 并发送（最后一帧）
        zmq::message_t payloadMsg(payload.size());
        if (!payload.empty()) {
            std::memcpy(payloadMsg.data(), payload.c_str(), payload.size());
        }

        result = publisher_->send(payloadMsg, zmq::send_flags::none);
        if (!result) {
            return MqResult::fail(MqErrorCode::SendFailed,
                "Failed to send payload: type=" + std::to_string(header.type));
        }

        // [DEBUG] payload 发送成功
        log(LogLevel::Info, "[SEND] payload sent OK, type=" + std::to_string(header.type));

        return MqResult::success();
    } catch (const zmq::error_t& e) {
        return MqResult::fail(MqErrorCode::ZmqError,
            "Send failed: " + std::string(e.what()));
    }
}

/**
 * 订阅指定类型的消息（库内置类型）
 *
 * 线程安全地将回调函数注册到回调表中。
 * 同一消息类型重复订阅会覆盖之前的回调。
 *
 * @param type 消息类型
 * @param callback 回调函数
 */
void InprocQueue::subscribe(InprocMessageType type, MessageCallback callback) {
    subscribe(static_cast<uint32_t>(type), std::move(callback));
}

/**
 * 订阅指定类型的消息（通用接口，支持自定义类型）
 *
 * 允许应用订阅自定义消息类型（10000-65535 范围）。
 * 线程安全地将回调函数注册到回调表中。
 *
 * @param type 消息类型（可以是库内置或应用自定义类型）
 * @param callback 回调函数
 */
void InprocQueue::subscribe(uint32_t type, MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callbacks_[type] = std::move(callback);
}

void InprocQueue::clearAll() {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callbacks_.clear();
}

/**
 * 取消订阅指定类型的消息
 *
 * 线程安全地从回调表中移除指定类型的回调。
 *
 * @param type 消息类型
 */
void InprocQueue::unsubscribe(InprocMessageType type) {
    unsubscribe(static_cast<uint32_t>(type));
}

/**
 * 取消订阅指定类型的消息（通用接口，支持自定义类型）
 *
 * @param type 消息类型
 */
void InprocQueue::unsubscribe(uint32_t type) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callbacks_.erase(type);
}

/**
 * 输出日志
 *
 * 将日志消息传递给外部回调函数。
 * 如果未设置回调函数，则丢弃日志。
 *
 * @param level 日志级别
 * @param msg 日志消息内容
 */
void InprocQueue::log(LogLevel level, const std::string& msg) {
    if (logCallback_) {
        logCallback_(level, msg);
    }
}

/**
 * 接收循环（运行在独立线程中）
 *
 * 持续从 SUBSCRIBER socket 接收两帧消息：
 * 第一帧：MessageHeader 二进制
 * 第二帧：payload JSON 字符串
 * 
 * 接收超时由 rcvtimeo 控制（默认 1000ms），超时后继续下一轮循环。
 * 当 running_ 为 false 时退出循环。
 */
void InprocQueue::receiveLoop() {
    log(LogLevel::Info, "InprocQueue receiveLoop started: " + endpoint_);

    while (running_) {
        try {
            // 步骤 1：接收消息头帧
            zmq::message_t headerMsg;
            auto result = subscriber_->recv(headerMsg, zmq::recv_flags::none);

            // 检查是否应该退出
            if (!running_) break;

            // 超时或消息头大小不匹配，跳过本次循环
            if (!result || headerMsg.size() != sizeof(MessageHeader)) continue;

            // 步骤 2：反序列化消息头
            MessageHeader header;
            std::memcpy(&header, headerMsg.data(), sizeof(MessageHeader));

            // 步骤 3：接收 payload 帧
            zmq::message_t payloadMsg;
            result = subscriber_->recv(payloadMsg, zmq::recv_flags::none);

            // 再次检查退出条件
            if (!running_) break;
            if (!result) continue;

            // 步骤 4：将 payload 帧转为字符串并分发
            std::string payload(static_cast<const char*>(payloadMsg.data()), payloadMsg.size());
            
            // [DEBUG] 记录接收
            log(LogLevel::Info, "[RECV] type=" + std::to_string(header.type) + ", payload_len=" + std::to_string(payload.length()));
            
            dispatchMessage(header, payload);

        } catch (const zmq::error_t& e) {
            // ZMQ 错误：EAGAIN 是超时（正常），其他错误需要记录
            if (!running_) break;
            if (e.num() != EAGAIN) {
                log(LogLevel::Error, "ZMQ error: " + std::string(e.what()));
            }
        } catch (const std::exception& e) {
            // 其他异常：记录后继续循环（不崩溃）
            if (!running_) break;
            log(LogLevel::Error, "Exception: " + std::string(e.what()));
        }
    }

    log(LogLevel::Info, "InprocQueue receiveLoop exited");
}

/**
 * 分发消息到对应回调函数
 *
 * 根据消息头中的类型字段查找已注册的回调。
 * 找到则调用回调，未找到则丢弃消息。
 * 回调异常不会传播，只记录日志。
 *
 * @param header 消息头
 * @param payload 消息负载（JSON 字符串）
 */
void InprocQueue::dispatchMessage(const MessageHeader& header, const std::string& payload) {
    std::lock_guard<std::mutex> lock(callbackMutex_);

    // 直接使用消息头中的类型值查找对应的回调（支持库内置和自定义类型）
    auto it = callbacks_.find(header.type);

    if (it != callbacks_.end()) {
        try {
            // 调用注册的回调函数处理消息
            it->second(header, payload);
        } catch (const std::exception& e) {
            // 回调异常：记录日志但不影响其他消息处理
            log(LogLevel::Error, "Callback error for type " + std::to_string(header.type));
        }
    } else {
        // [DEBUG] 未找到回调
        log(LogLevel::Warn, "[DISPATCH] NO callback found, type=" + std::to_string(header.type));
    }
}

}
