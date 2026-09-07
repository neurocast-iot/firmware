/**
 * message_manager.cpp
 * 统一消息管理器实现
 *
 * 封装 InprocQueue 和 IpcQueue，对外提供统一的消息发送/订阅接口。
 * 使用 Builder 模式构建，自动初始化内部队列。
 */
#include "libmq/message_manager.h"
#include "libmq/inproc_queue.h"
#include "libmq/ipc_queue.h"
#include <cstring>
#include <cstdio>

namespace libmq {

/**
 * Builder::build() - 构建 MessageManager 实例
 *
 * 根据配置创建并初始化内部队列。
 * 如果 inprocEndpoint 为空或初始化失败，返回 nullptr。
 *
 * @return MessageManager 智能指针，失败返回 nullptr
 */
std::unique_ptr<MessageManager> MessageManager::Builder::build() {
    // 步骤 1：验证配置是否合法（inprocEndpoint 不能为空）
    if (!config_.isValid()) {
        return nullptr;
    }

    // 步骤 2：创建 MessageManager 实例（私有构造函数，只能由 Builder 创建）
    return std::unique_ptr<MessageManager>(new MessageManager(config_));
}

/**
 * MessageManager 构造函数
 *
 * 根据配置创建并初始化 InprocQueue（必选）和 IpcQueue（可选）。
 * 构造函数内部失败不会抛异常，而是保持 initialized_ = false。
 *
 * @param config 消息队列配置
 */
MessageManager::MessageManager(const MqConfig& config)
    : config_(config) {
    try {
        // 步骤 1：创建并初始化进程内消息队列（必选）
        inprocQueue_ = std::make_unique<InprocQueue>();
        if (!inprocQueue_->initialize(config_.inprocEndpoint, config_)) {
            return;  // 初始化失败，保持 initialized_ = false
        }

        // 步骤 2：如果配置了 IPC 端点，创建并初始化进程间消息队列（可选）
        if (!config_.ipcEndpoint.empty()) {
            ipcQueue_ = std::make_unique<IpcQueue>();

            // 根据 IPC 通信模式选择初始化方法
            bool ok = false;
            switch (config_.ipcMode) {
                case IpcMode::PairServer:
                    // PAIR 服务端：bind 端点，等待客户端连接
                    ok = ipcQueue_->initializeAsServer(config_.ipcEndpoint, config_);
                    break;
                case IpcMode::PairClient:
                    // PAIR 客户端：connect 到服务端
                    ok = ipcQueue_->initializeAsClient(config_.ipcEndpoint, config_);
                    break;
                case IpcMode::Publisher:
                    // PUB 发布端：bind 端点，一对多广播
                    ok = ipcQueue_->initializeAsPublisher(config_.ipcEndpoint, config_);
                    break;
                case IpcMode::Subscriber:
                    // SUB 订阅端：connect 到发布端，接收消息
                    ok = ipcQueue_->initializeAsSubscriber(config_.ipcEndpoint, config_);
                    break;
                case IpcMode::Router:
                    // ROUTER 服务端：bind 端点，多对多双向（中心节点）
                    ok = ipcQueue_->initializeAsRouter(config_.ipcEndpoint, config_);
                    break;
                case IpcMode::Dealer:
                    // DEALER 客户端：connect 到 ROUTER，多对多双向（边缘节点）
                    ok = ipcQueue_->initializeAsDealer(config_.ipcEndpoint, config_);
                    break;
            }
            
            if (!ok) {
                return;  // IPC 初始化失败，保持 initialized_ = false
            }
        }

        initialized_ = true;
    } catch (...) {
        // 构造函数异常：保持 initialized_ = false
        initialized_ = false;
    }
}

/**
 * 析构函数
 *
 * 自动调用 stop() 完成所有清理工作。
 */
MessageManager::~MessageManager() {
    stop();
}

/**
 * 启动消息管理器
 *
 * 依次启动进程内队列和进程间队列（如果有），启动各自的接收线程。
 * 必须在 build() 成功后调用。
 *
 * @return MqResult 操作结果
 */
MqResult MessageManager::start() {
    // 检查前置条件：必须先初始化
    if (!initialized_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "MessageManager not initialized");
    }

    // 防止重复启动
    if (running_) {
        return MqResult::success();
    }

    try {
        // 步骤 1：启动进程内消息队列（接收线程）
        if (!inprocQueue_->start()) {
            return MqResult::fail(MqErrorCode::NotInitialized, "InprocQueue start failed");
        }

        // 步骤 2：启动进程间消息队列（如果有，接收线程）
        if (ipcQueue_ && !ipcQueue_->start()) {
            return MqResult::fail(MqErrorCode::NotInitialized, "IpcQueue start failed");
        }

        running_ = true;
        return MqResult::success();
    } catch (const std::exception& e) {
        return MqResult::fail(MqErrorCode::ZmqError, e.what());
    }
}

/**
 * 停止消息管理器
 *
 * 取消所有订阅，停止所有消息队列，关闭接收线程和 socket。
 * 这是完整的清理方法，析构函数会自动调用。
 */
void MessageManager::stop() {
    // 如果已经停止，直接返回（避免重复操作）
    if (!running_ && !initialized_) {
        return;
    }

    // 步骤 1：取消所有订阅
    clearAll();
    
    // 步骤 2：停止接收线程
    running_ = false;
    try {
        if (inprocQueue_) {
            inprocQueue_->stop();
        }
        if (ipcQueue_) {
            ipcQueue_->stop();
        }
    } catch (...) {
        // 停止过程中异常不影响整体清理
    }
    
    // 步骤 3：标记为未初始化
    initialized_ = false;
}

/**
 * 发送进程内消息（库内置类型）
 *
 * 自动构建 MessageHeader 并填充 INPROC 域、消息类型、时间戳和序列号，
 * 然后通过 InprocQueue 发送。
 *
 * @param type 消息类型（InprocMessageType 枚举值）
 * @param payload 消息负载（JSON 格式字符串），默认为空
 * @return MqResult 操作结果
 */
MqResult MessageManager::send(InprocMessageType type, const std::string& payload) {
    // 检查前置条件：InprocQueue 必须存在且管理器正在运行
    if (!inprocQueue_ || !running_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "InprocQueue not ready");
    }

    try {
        // 步骤 1：构建消息头（自动填充域、时间戳、序列号）
        MessageHeader header;
        header.domain = MessageDomain::INPROC;            // 标记为进程内消息
        header.type = static_cast<uint32_t>(type);        // 消息类型
        header.timestamp = getCurrentTimestamp();         // 当前时间戳
        header.sequence = ++sequence_;                    // 全局序列号（原子递增）
        header.payloadSize = static_cast<uint32_t>(payload.size());

        // 步骤 2：通过 InprocQueue 发送
        return inprocQueue_->send(header, payload);
    } catch (const std::exception& e) {
        return MqResult::fail(MqErrorCode::SendFailed, e.what());
    }
}

/**
 * 发送进程间消息（库内置类型）
 *
 * 自动构建 MessageHeader 并填充 IPC 域、消息类型、时间戳和序列号，
 * 然后通过 IpcQueue 发送给对端进程。
 * 如果未初始化 IPC 队列，则返回失败。
 *
 * @param type 消息类型（IpcMessageType 枚举值）
 * @param payload 消息负载（JSON 格式字符串），默认为空
 * @return MqResult 操作结果
 */
MqResult MessageManager::send(IpcMessageType type, const std::string& payload) {
    // 检查前置条件：IpcQueue 必须存在且管理器正在运行
    if (!ipcQueue_ || !running_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "IpcQueue not initialized");
    }

    try {
        // 步骤 1：构建消息头（自动填充域、时间戳、序列号）
        MessageHeader header;
        header.domain = MessageDomain::IPC;               // 标记为进程间消息
        header.type = static_cast<uint32_t>(type);        // 消息类型
        header.timestamp = getCurrentTimestamp();         // 当前时间戳
        header.sequence = ++sequence_;                    // 全局序列号（原子递增）
        header.payloadSize = static_cast<uint32_t>(payload.size());

        // 步骤 2：通过 IpcQueue 发送
        return ipcQueue_->send(header, payload);
    } catch (const std::exception& e) {
        return MqResult::fail(MqErrorCode::SendFailed, e.what());
    }
}

/**
 * 发送进程内消息（通用接口，支持自定义类型）
 *
 * 允许应用使用自定义消息类型（10000-65535 范围）。
 * 自动构建 MessageHeader 并填充 INPROC 域、消息类型、时间戳和序列号。
 *
 * @param type 消息类型（可以是库内置或应用自定义类型）
 * @param payload 消息负载（JSON 格式字符串），默认为空
 * @return MqResult 操作结果
 */
MqResult MessageManager::send(uint32_t type, const std::string& payload) {
    // 复用 InprocMessageType 重载版本，直接传递 uint32_t
    // 由于 MessageHeader::type 本身就是 uint32_t，无需转换
    if (!inprocQueue_ || !running_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "InprocQueue not ready");
    }

    try {
        MessageHeader header;
        header.domain = MessageDomain::INPROC;
        header.type = type;                               // 直接使用传入的 uint32_t 类型
        header.timestamp = getCurrentTimestamp();
        header.sequence = ++sequence_;
        header.payloadSize = static_cast<uint32_t>(payload.size());

        return inprocQueue_->send(header, payload);
    } catch (const std::exception& e) {
        return MqResult::fail(MqErrorCode::SendFailed, e.what());
    }
}

/**
 * 发送进程间消息（通用接口，支持自定义类型）
 *
 * 允许应用使用自定义消息类型（10000-65535 范围）。
 * 自动构建 MessageHeader 并填充 IPC 域、消息类型、时间戳和序列号。
 *
 * @param type 消息类型（可以是库内置或应用自定义类型）
 * @param payload 消息负载（JSON 格式字符串），默认为空
 * @return MqResult 操作结果
 */
MqResult MessageManager::sendIpc(uint32_t type, const std::string& payload) {
    if (!ipcQueue_ || !running_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "IpcQueue not initialized");
    }

    try {
        MessageHeader header;
        header.domain = MessageDomain::IPC;
        header.type = type;
        header.timestamp = getCurrentTimestamp();
        header.sequence = ++sequence_;
        header.payloadSize = static_cast<uint32_t>(payload.size());

        return ipcQueue_->send(header, payload);
    } catch (const std::exception& e) {
        return MqResult::fail(MqErrorCode::SendFailed, e.what());
    }
}

/**
 * 发送消息给指定目标（ROUTER 模式用）
 *
 * ROUTER 模式下，根据目标 peer 名字查找其 ZMQ 身份标识，然后发送。
 */
MqResult MessageManager::sendTo(const std::string& targetName, uint32_t type, const std::string& payload) {
    if (!ipcQueue_ || !running_ || !ipcQueue_->isRouter()) {
        return MqResult::fail(MqErrorCode::NotInitialized, "IpcQueue not in ROUTER mode");
    }

    /* 查找目标 peer 的 ZMQ 身份标识
     * DEALER 连接时把自己的名字设为 socket identity，
     * 所以直接用 targetName 作为 identity 就行（不用等注册消息） */
    std::string identity = ipcQueue_->getPeerIdentity(targetName);
    if (identity.empty()) {
        /* 映射表里没有，但 DEALER 的 identity 就是它的名字，直接用 */
        identity = targetName;
    }

    try {
        MessageHeader header;
        header.domain = MessageDomain::IPC;
        header.type = type;
        header.timestamp = getCurrentTimestamp();
        header.sequence = ++sequence_;
        header.payloadSize = static_cast<uint32_t>(payload.size());
        std::strncpy(header.from, config_.peerName.c_str(), MessageHeader::kNameMaxLen - 1);
        header.from[MessageHeader::kNameMaxLen - 1] = '\0';
        std::strncpy(header.to, targetName.c_str(), MessageHeader::kNameMaxLen - 1);
        header.to[MessageHeader::kNameMaxLen - 1] = '\0';

        return ipcQueue_->sendTo(identity, header, payload);
    } catch (const std::exception& e) {
        return MqResult::fail(MqErrorCode::SendFailed, e.what());
    }
}

/**
 * 发送消息给指定目标（ROUTER 模式用，库内置消息类型）
 */
MqResult MessageManager::sendTo(const std::string& targetName, IpcMessageType type, const std::string& payload) {
    return sendTo(targetName, static_cast<uint32_t>(type), payload);
}

/**
 * 订阅进程内消息
 *
 * 在 InprocQueue 中注册指定类型消息的回调函数。
 * 回调会一直有效,直到调用 unsubscribe() 或 clearAll()。
 *
 * @param type 要订阅的消息类型
 * @param callback 消息到达时的回调函数
 */
void MessageManager::subscribe(InprocMessageType type, MessageCallback callback) {
    if (!inprocQueue_) {
        return;
    }

    inprocQueue_->subscribe(type, std::move(callback));
}

/**
 * 订阅进程间消息
 *
 * 在 IpcQueue 中注册指定类型消息的回调函数。
 * 如果未初始化 IPC 队列,则忽略。
 *
 * @param type 要订阅的消息类型
 * @param callback 消息到达时的回调函数
 */
void MessageManager::subscribe(IpcMessageType type, MessageCallback callback) {
    if (!ipcQueue_) {
        return;
    }

    ipcQueue_->subscribe(type, std::move(callback));
}

/**
 * 订阅进程内消息（通用接口，支持自定义类型）
 *
 * 允许应用订阅自定义消息类型（10000-65535 范围）。
 *
 * @param type 要订阅的消息类型（可以是库内置或应用自定义类型）
 * @param callback 消息到达时的回调函数
 */
void MessageManager::subscribe(uint32_t type, MessageCallback callback) {
    if (!inprocQueue_) {
        return;
    }

    // InprocQueue::subscribe 接受 InprocMessageType 枚举
    // 需要将 uint32_t 转换为枚举类型（自定义类型也在同一范围内）
    inprocQueue_->subscribe(static_cast<InprocMessageType>(type), std::move(callback));
}

/**
 * 订阅进程间消息（通用接口，支持自定义类型）
 *
 * 允许应用订阅自定义消息类型（10000-65535 范围）。
 *
 * @param type 要订阅的消息类型（可以是库内置或应用自定义类型）
 * @param callback 消息到达时的回调函数
 */
void MessageManager::subscribeIpc(uint32_t type, MessageCallback callback) {
    if (!ipcQueue_) {
        return;
    }

    ipcQueue_->subscribe(static_cast<IpcMessageType>(type), std::move(callback));
}

/**
 * 取消所有订阅
 *
 * 清空 InprocQueue 和 IpcQueue 中所有已注册的回调函数。
 * 通常在程序退出时调用。
 */
void MessageManager::clearAll() {
    if (inprocQueue_) {
        inprocQueue_->clearAll();
    }
    if (ipcQueue_) {
        ipcQueue_->clearAll();
    }
}

}
