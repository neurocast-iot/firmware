/**
 * ipc_queue.cpp
 * 进程间消息队列实现
 *
 * 使用 ZeroMQ 的 IPC 传输方式（Unix Domain Socket）实现跨进程消息传递。
 * 采用 PAIR 模式：一对一双向通信，适用于固定配对的两个进程。
 * 从全局 Context 创建 socket，所有队列共享同一个 zmq::context_t。
 */
#include "libmq/ipc_queue.h"
#include "libmq/zmq_context.h"
#include <cstring>

namespace libmq {

/**
 * 构造函数
 *
 * 使用 default，所有成员变量在头文件中已初始化为默认值。
 */
IpcQueue::IpcQueue() = default;

/**
 * 析构函数
 *
 * 自动调用 stop() 停止接收线程并释放 socket 资源。
 */
IpcQueue::~IpcQueue() {
    stop();
}

/**
 * 初始化进程间消息队列（服务端模式）
 *
 * 从全局 ZmqContextManager 创建 PAIR socket 并绑定到指定 IPC 端点。
 * 服务端负责创建 IPC socket 文件，客户端连接到该端点。
 * 同时配置接收/发送超时和高水位参数。
 *
 * @param endpoint IPC 端点地址，格式为 "ipc://socket文件路径"
 * @param config 配置参数
 * @return true=初始化成功, false=端点已被占用或权限不足
 */
bool IpcQueue::initializeAsServer(const std::string& endpoint, const MqConfig& config) {
    try {
        endpoint_ = endpoint;
        isServer_ = true;
        logCallback_ = config.logCallback;
        recvTimeoutMs_ = config.recvTimeoutMs;
        sendTimeoutMs_ = config.sendTimeoutMs;

        // 从全局 Context 创建 socket（整个进程共享同一个 context）
        auto& ctx = ZmqContextManager::instance();

        // 使用 PAIR 模式：一对一双向通信
        socket_ = new zmq::socket_t(ctx, ZMQ_PAIR);

        // 服务端 bind 到指定端点（创建 IPC socket 文件）
        socket_->bind(endpoint_);

        // 设置接收超时，避免接收线程无限阻塞
        socket_->set(zmq::sockopt::rcvtimeo, recvTimeoutMs_);

        // 设置发送超时，防止发送线程无限阻塞
        socket_->set(zmq::sockopt::sndtimeo, sendTimeoutMs_);

        // 设置 linger 为 0：关闭 socket 时立即丢弃未发送的消息
        socket_->set(zmq::sockopt::linger, 0);

        // 配置高水位（可选），防止消息堆积导致内存耗尽
        if (config.sendHwm > 0) {
            socket_->set(zmq::sockopt::sndhwm, config.sendHwm);
        }
        if (config.recvHwm > 0) {
            socket_->set(zmq::sockopt::rcvhwm, config.recvHwm);
        }

        initialized_ = true;
        log(LogLevel::Info, "IpcQueue initialized as server: " + endpoint_);
        return true;
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "IpcQueue server init failed: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        log(LogLevel::Error, "IpcQueue server init failed: " + std::string(e.what()));
        return false;
    }
}

/**
 * 初始化进程间消息队列（客户端模式）
 *
 * 从全局 ZmqContextManager 创建 PAIR socket 并连接到服务端端点。
 * 客户端不创建 socket 文件，只负责连接。
 *
 * @param endpoint IPC 端点地址，需与服务端绑定的端点一致
 * @param config 配置参数
 * @return true=初始化成功, false=服务端未启动或端点不存在
 */
bool IpcQueue::initializeAsClient(const std::string& endpoint, const MqConfig& config) {
    try {
        endpoint_ = endpoint;
        isServer_ = false;
        logCallback_ = config.logCallback;
        recvTimeoutMs_ = config.recvTimeoutMs;
        sendTimeoutMs_ = config.sendTimeoutMs;

        // 从全局 Context 创建 socket（整个进程共享同一个 context）
        auto& ctx = ZmqContextManager::instance();

        // 使用 PAIR 模式：一对一双向通信
        socket_ = new zmq::socket_t(ctx, ZMQ_PAIR);

        // 客户端 connect 到服务端创建的端点
        socket_->connect(endpoint_);

        // 设置接收超时
        socket_->set(zmq::sockopt::rcvtimeo, recvTimeoutMs_);

        // 设置发送超时
        socket_->set(zmq::sockopt::sndtimeo, sendTimeoutMs_);

        // 设置 linger 为 0
        socket_->set(zmq::sockopt::linger, 0);

        // 配置高水位
        if (config.sendHwm > 0) {
            socket_->set(zmq::sockopt::sndhwm, config.sendHwm);
        }
        if (config.recvHwm > 0) {
            socket_->set(zmq::sockopt::rcvhwm, config.recvHwm);
        }

        initialized_ = true;
        log(LogLevel::Info, "IpcQueue initialized as client: " + endpoint_);
        return true;
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "IpcQueue client init failed: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        log(LogLevel::Error, "IpcQueue client init failed: " + std::string(e.what()));
        return false;
    }
}

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
bool IpcQueue::initializeAsPublisher(const std::string& endpoint, const MqConfig& config) {
    try {
        endpoint_ = endpoint;
        isServer_ = true;  // 发布者绑定端点（类似服务端）
        recvSupported_ = false;  // PUB 只发不收，start() 据此跳过接收线程
        logCallback_ = config.logCallback;
        recvTimeoutMs_ = config.recvTimeoutMs;
        sendTimeoutMs_ = config.sendTimeoutMs;

        // 从全局 Context 创建 socket
        auto& ctx = ZmqContextManager::instance();

        // 使用 PUB 模式：一对多发布
        socket_ = new zmq::socket_t(ctx, ZMQ_PUB);

        // 发布者 bind 到指定端点（创建 IPC socket 文件）
        socket_->bind(endpoint_);

        // 设置发送超时
        socket_->set(zmq::sockopt::sndtimeo, sendTimeoutMs_);

        // 设置 linger 为 0：关闭 socket 时立即丢弃未发送的消息
        socket_->set(zmq::sockopt::linger, 0);

        // 配置高水位
        if (config.sendHwm > 0) {
            socket_->set(zmq::sockopt::sndhwm, config.sendHwm);
        }

        initialized_ = true;
        log(LogLevel::Info, "IpcQueue initialized as publisher: " + endpoint_);
        return true;
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "IpcQueue publisher init failed: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        log(LogLevel::Error, "IpcQueue publisher init failed: " + std::string(e.what()));
        return false;
    }
}

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
bool IpcQueue::initializeAsSubscriber(const std::string& endpoint, const MqConfig& config) {
    try {
        endpoint_ = endpoint;
        isServer_ = false;  // 订阅者连接端点（类似客户端）
        logCallback_ = config.logCallback;
        recvTimeoutMs_ = config.recvTimeoutMs;
        sendTimeoutMs_ = config.sendTimeoutMs;

        // 从全局 Context 创建 socket
        auto& ctx = ZmqContextManager::instance();

        // 使用 SUB 模式：订阅发布者的消息
        socket_ = new zmq::socket_t(ctx, ZMQ_SUB);

        // 订阅者 connect 到发布者创建的端点
        socket_->connect(endpoint_);

        // 设置接收超时，避免接收线程无限阻塞
        socket_->set(zmq::sockopt::rcvtimeo, recvTimeoutMs_);

        // 订阅所有消息（空字符串表示不过滤）
        socket_->set(zmq::sockopt::subscribe, "");

        // 配置高水位
        if (config.recvHwm > 0) {
            socket_->set(zmq::sockopt::rcvhwm, config.recvHwm);
        }

        initialized_ = true;
        log(LogLevel::Info, "IpcQueue initialized as subscriber: " + endpoint_);
        return true;
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "IpcQueue subscriber init failed: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        log(LogLevel::Error, "IpcQueue subscriber init failed: " + std::string(e.what()));
        return false;
    }
}

/**
 * 初始化进程间消息队列（ROUTER 模式 - 中心节点）
 *
 * 使用 ZMQ_ROUTER socket，bind 到指定端点。
 * 支持多个 DEALER 客户端同时连接，实现多对多双向通信。
 */
bool IpcQueue::initializeAsRouter(const std::string& endpoint, const MqConfig& config) {
    try {
        endpoint_ = endpoint;
        peerName_ = config.peerName;
        isServer_ = true;
        isRouterMode_ = true;
        logCallback_ = config.logCallback;
        recvTimeoutMs_ = config.recvTimeoutMs;
        sendTimeoutMs_ = config.sendTimeoutMs;

        auto& ctx = ZmqContextManager::instance();
        socket_ = new zmq::socket_t(ctx, ZMQ_ROUTER);
        socket_->bind(endpoint_);

        socket_->set(zmq::sockopt::rcvtimeo, recvTimeoutMs_);
        socket_->set(zmq::sockopt::sndtimeo, sendTimeoutMs_);
        socket_->set(zmq::sockopt::linger, 0);

        if (config.sendHwm > 0) {
            socket_->set(zmq::sockopt::sndhwm, config.sendHwm);
        }
        if (config.recvHwm > 0) {
            socket_->set(zmq::sockopt::rcvhwm, config.recvHwm);
        }

        initialized_ = true;
        log(LogLevel::Info, "IpcQueue initialized as ROUTER: " + endpoint_ + " (name=" + peerName_ + ")");
        return true;
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "IpcQueue ROUTER init failed: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        log(LogLevel::Error, "IpcQueue ROUTER init failed: " + std::string(e.what()));
        return false;
    }
}

/**
 * 初始化进程间消息队列（DEALER 模式 - 边缘节点）
 *
 * 使用 ZMQ_DEALER socket，connect 到 ROUTER 的 IPC 端点。
 * 多个 DEALER 可连接到同一个 ROUTER，实现多对多双向通信。
 */
bool IpcQueue::initializeAsDealer(const std::string& endpoint, const MqConfig& config) {
    try {
        endpoint_ = endpoint;
        peerName_ = config.peerName;
        isServer_ = false;
        isDealerMode_ = true;
        logCallback_ = config.logCallback;
        recvTimeoutMs_ = config.recvTimeoutMs;
        sendTimeoutMs_ = config.sendTimeoutMs;

        auto& ctx = ZmqContextManager::instance();
        socket_ = new zmq::socket_t(ctx, ZMQ_DEALER);
        
        /* 设置 DEALER 的身份标识，ROUTER 用这个标识发送消息给本节点
         * 必须在 connect 之前设置，否则 ROUTER 不知道我是谁 */
        if (!peerName_.empty()) {
            socket_->set(zmq::sockopt::routing_id, peerName_);
        }
        
        socket_->connect(endpoint_);

        socket_->set(zmq::sockopt::rcvtimeo, recvTimeoutMs_);
        socket_->set(zmq::sockopt::sndtimeo, sendTimeoutMs_);
        socket_->set(zmq::sockopt::linger, 0);

        if (config.sendHwm > 0) {
            socket_->set(zmq::sockopt::sndhwm, config.sendHwm);
        }
        if (config.recvHwm > 0) {
            socket_->set(zmq::sockopt::rcvhwm, config.recvHwm);
        }

        initialized_ = true;
        log(LogLevel::Info, "IpcQueue initialized as DEALER: " + endpoint_ + " (name=" + peerName_ + ")");
        return true;
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "IpcQueue DEALER init failed: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        log(LogLevel::Error, "IpcQueue DEALER init failed: " + std::string(e.what()));
        return false;
    }
}

/**
 * 启动消息队列
 *
 * 启动独立接收线程，开始监听 PAIR socket 并分发消息。
 * 必须在 initialize() 成功后调用。
 *
 * @return MqResult 操作结果
 */
MqResult IpcQueue::start() {
    // 防止重复启动
    if (running_) {
        log(LogLevel::Warn, "IpcQueue already running");
        return MqResult::success();
    }

    // 检查前置条件：必须先初始化
    if (!initialized_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "IpcQueue not initialized");
    }

    running_ = true;

    // 启动接收线程，独立运行 receiveLoop()
    // Publisher（ZMQ_PUB）只发不收：对 PUB 调 recv 会立即抛 ENOTSUP
    //（不等超时），接收循环会变成刷日志的忙循环，故直接不拉线程
    if (recvSupported_) {
        receiveThread_ = std::make_unique<std::thread>(&IpcQueue::receiveLoop, this);
    } else {
        log(LogLevel::Info, "IpcQueue publisher mode: no receive loop");
    }

    log(LogLevel::Info, "IpcQueue started");
    return MqResult::success();
}

/**
 * 停止消息队列
 *
 * 设置运行标志为 false 通知接收线程退出，
 * 等待线程 join，然后删除 socket 释放资源。
 */
void IpcQueue::stop() {
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

    // 步骤 3：释放 PAIR socket
    if (socket_) {
        delete socket_;
        socket_ = nullptr;
    }

    log(LogLevel::Info, "IpcQueue stopped");
}

/**
 * 发送消息
 *
 * 通过 PAIR socket 发送两帧消息：
 * 第一帧：MessageHeader 二进制数据（带 sndmore 标志）
 * 第二帧：payload JSON 字符串
 * 发送超时为配置的 sendTimeoutMs，超时则认为对端未就绪。
 *
 * @param header 消息头
 * @param payload 消息负载（JSON 字符串）
 * @return MqResult 操作结果
 */
MqResult IpcQueue::send(const MessageHeader& header, const std::string& payload) {
    // 检查前置条件：socket 必须存在且队列正在运行
    if (!socket_ || !running_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "IpcQueue not running");
    }

    // DEALER 模式：自动填充 from 字段
    MessageHeader hdr = header;
    if (isDealerMode_ && hdr.from[0] == '\0') {
        std::strncpy(hdr.from, peerName_.c_str(), MessageHeader::kNameMaxLen - 1);
        hdr.from[MessageHeader::kNameMaxLen - 1] = '\0';
    }

    try {
        // 步骤 1：序列化消息头为 zmq::message_t 并发送（sndmore 表示还有后续帧）
        // 使用 dontwait 标志：如果发送缓冲区满则立即返回失败，避免阻塞
        zmq::message_t headerMsg(sizeof(MessageHeader));
        std::memcpy(headerMsg.data(), &hdr, sizeof(MessageHeader));

        auto result = socket_->send(headerMsg,
            zmq::send_flags::sndmore | zmq::send_flags::dontwait);
        if (!result) {
            return MqResult::fail(MqErrorCode::SendFailed,
                "Failed to send header");
        }

        // 步骤 2：序列化 payload 为 zmq::message_t 并发送（最后一帧）
        zmq::message_t payloadMsg(payload.size());
        if (!payload.empty()) {
            std::memcpy(payloadMsg.data(), payload.c_str(), payload.size());
        }

        result = socket_->send(payloadMsg, zmq::send_flags::dontwait);
        if (!result) {
            return MqResult::fail(MqErrorCode::SendFailed,
                "Failed to send payload");
        }

        return MqResult::success();
    } catch (const zmq::error_t& e) {
        return MqResult::fail(MqErrorCode::ZmqError,
            "Send failed: " + std::string(e.what()));
    }
}

/**
 * 发送消息给指定目标（ROUTER 模式用）
 *
 * ROUTER 模式下，需要指定目标 peer 的身份标识才能发送。
 * 消息格式：[targetIdentity][empty][header][payload]
 */
MqResult IpcQueue::sendTo(const std::string& targetIdentity, const MessageHeader& header, const std::string& payload) {
    if (!socket_ || !running_ || !isRouterMode_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "IpcQueue not in ROUTER mode");
    }

    try {
        // 步骤 1：发送目标身份帧（ROUTER 用这个决定发给谁）
        zmq::message_t identityMsg(targetIdentity.size());
        std::memcpy(identityMsg.data(), targetIdentity.data(), targetIdentity.size());
        auto result = socket_->send(identityMsg, zmq::send_flags::sndmore | zmq::send_flags::dontwait);
        if (!result) {
            return MqResult::fail(MqErrorCode::SendFailed, "Failed to send identity");
        }

        // 步骤 2：发送空帧（ZMQ 协议要求）
        zmq::message_t emptyMsg(0);
        result = socket_->send(emptyMsg, zmq::send_flags::sndmore | zmq::send_flags::dontwait);
        if (!result) {
            return MqResult::fail(MqErrorCode::SendFailed, "Failed to send empty");
        }

        // 步骤 3：发送消息头
        MessageHeader hdr = header;
        if (hdr.from[0] == '\0') {
            std::strncpy(hdr.from, peerName_.c_str(), MessageHeader::kNameMaxLen - 1);
            hdr.from[MessageHeader::kNameMaxLen - 1] = '\0';
        }
        zmq::message_t headerMsg(sizeof(MessageHeader));
        std::memcpy(headerMsg.data(), &hdr, sizeof(MessageHeader));
        result = socket_->send(headerMsg, zmq::send_flags::sndmore | zmq::send_flags::dontwait);
        if (!result) {
            return MqResult::fail(MqErrorCode::SendFailed, "Failed to send header");
        }

        // 步骤 4：发送 payload
        zmq::message_t payloadMsg(payload.size());
        if (!payload.empty()) {
            std::memcpy(payloadMsg.data(), payload.c_str(), payload.size());
        }
        result = socket_->send(payloadMsg, zmq::send_flags::dontwait);
        if (!result) {
            return MqResult::fail(MqErrorCode::SendFailed, "Failed to send payload");
        }

        return MqResult::success();
    } catch (const zmq::error_t& e) {
        return MqResult::fail(MqErrorCode::ZmqError, "SendTo failed: " + std::string(e.what()));
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
void IpcQueue::subscribe(IpcMessageType type, MessageCallback callback) {
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
void IpcQueue::subscribe(uint32_t type, MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callbacks_[type] = std::move(callback);
}

void IpcQueue::clearAll() {
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
void IpcQueue::unsubscribe(IpcMessageType type) {
    unsubscribe(static_cast<uint32_t>(type));
}

/**
 * 取消订阅指定类型的消息（通用接口，支持自定义类型）
 *
 * @param type 消息类型
 */
void IpcQueue::unsubscribe(uint32_t type) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callbacks_.erase(type);
}

// ===== 日志方法 =====

void IpcQueue::log(LogLevel level, const std::string& msg) {
    if (logCallback_) {
        logCallback_(level, msg);
    }
}

/**
 * 接收循环（运行在独立线程中）
 *
 * 持续从 socket 接收消息：
 * - PAIR/DEALER 模式：[header][payload]
 * - ROUTER 模式：[senderIdentity][empty][header][payload]
 * 
 * 接收超时由 rcvtimeo 控制（默认 1000ms），超时后继续下一轮循环。
 * 当 running_ 为 false 时退出循环。
 */
void IpcQueue::receiveLoop() {
    log(LogLevel::Info, "IpcQueue receiveLoop started: " + endpoint_);
    
    while (running_) {
        try {
            std::string senderIdentity;
            
            // ROUTER 模式：先收身份帧和空帧
            if (isRouterMode_) {
                // 步骤 1：接收发送方身份帧
                zmq::message_t identityMsg;
                auto result = socket_->recv(identityMsg, zmq::recv_flags::none);
                if (!running_) break;
                if (!result) continue;
                
                senderIdentity = std::string(static_cast<char*>(identityMsg.data()), identityMsg.size());
                
                // 步骤 2：接收空帧
                zmq::message_t emptyMsg;
                result = socket_->recv(emptyMsg, zmq::recv_flags::none);
                if (!running_) break;
                if (!result) continue;
                // 兼容 DEALER 对端没发空帧的情况：
                // 如果对端直接发了 header，这里收到的就不是空帧，
                // 大小不为 0，直接把它当 header 处理，跳过后续接收
                if (emptyMsg.size() != 0) {
                    if (emptyMsg.size() == sizeof(MessageHeader)) {
                        MessageHeader header;
                        std::memcpy(&header, emptyMsg.data(), sizeof(MessageHeader));
                        zmq::message_t payloadMsg;
                        result = socket_->recv(payloadMsg, zmq::recv_flags::none);
                        if (!running_) break;
                        if (result) {
                            std::string payload(static_cast<const char*>(payloadMsg.data()), payloadMsg.size());
                            dispatchMessage(header, payload);
                        }
                    }
                    continue;
                }
            }
            
            // 步骤 3：接收消息头帧
            zmq::message_t headerMsg;
            auto result = socket_->recv(headerMsg, zmq::recv_flags::none);

            // 检查是否应该退出
            if (!running_) break;

            // 超时或消息头大小不匹配，跳过本次循环
            if (!result || headerMsg.size() != sizeof(MessageHeader)) continue;

            // 步骤 4：反序列化消息头
            MessageHeader header;
            std::memcpy(&header, headerMsg.data(), sizeof(MessageHeader));

            // 步骤 5：接收 payload 帧
            zmq::message_t payloadMsg;
            result = socket_->recv(payloadMsg, zmq::recv_flags::none);

            // 再次检查退出条件
            if (!running_) break;
            if (!result) continue;

            // 步骤 6：将 payload 帧转为字符串
            std::string payload(static_cast<const char*>(payloadMsg.data()), payloadMsg.size());
            
            // ROUTER 模式：记录 sender 的身份映射
            if (isRouterMode_ && !senderIdentity.empty() && header.from[0] != '\0') {
                std::lock_guard<std::mutex> lock(peerMutex_);
                peerIdentities_[std::string(header.from)] = senderIdentity;
                log(LogLevel::Debug, "ROUTER: mapped peer '" + std::string(header.from) + "' to identity");
            }
            
            // 步骤 7：分发消息
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
void IpcQueue::dispatchMessage(const MessageHeader& header, const std::string& payload) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    
    // 记录接收到的消息
    log(LogLevel::Info, "IpcQueue dispatchMessage: type=" + std::to_string(header.type) + 
                        ", len=" + std::to_string(payload.length()));

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
    }
}

}
