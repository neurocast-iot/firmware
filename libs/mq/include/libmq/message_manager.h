/**
 * message_manager.h
 * 统一消息管理器
 *
 * 管理进程内消息队列（InprocQueue）和进程间消息队列（IpcQueue），
 * 提供统一的消息发送和订阅接口。调用者无需关心底层是进程内还是进程间通信。
 * 使用 Builder 模式构建，RAII 管理生命周期。
 *
 * 内部使用 ZeroMQ 的 inproc 和 ipc 传输方式实现。
 *
 * 使用示例（PAIR 服务端 - iot_live）：
 *   auto manager = MessageManager::builder()
 *       .inprocEndpoint("inproc://main")
 *       .ipcEndpoint("ipc:///tmp/iot_live.ipc")
 *       .asPairServer()
 *       .build();
 *   manager->start();
 *   manager->send(InprocMessageType::PHOTO_CAPTURED, "{...}");
 *
 * 使用示例（PAIR 客户端 - ota_agent）：
 *   auto manager = MessageManager::builder()
 *       .inprocEndpoint("inproc://ota")
 *       .ipcEndpoint("ipc:///tmp/iot_live.ipc")
 *       .asPairClient()
 *       .build();
 *
 * 自定义消息类型使用示例：
 *   // 应用层定义自定义消息类型（10000-65535 范围）
 *   namespace my_app {
 *       enum class CustomInprocType : uint32_t {
 *           MOTION_DETECTED = 10000,
 *       };
 *   }
 *   // 使用通用 send() 接口发送
 *   manager->send(static_cast<uint32_t>(my_app::CustomInprocType::MOTION_DETECTED), payload);
 */
#pragma once
#include <memory>
#include <string>
#include "libmq/message_types.h"
#include "libmq/config.h"
#include "libmq/result.h"
#include "libmq/subscription.h"

namespace libmq {

class InprocQueue;
class IpcQueue;

/**
 * 统一消息管理器类
 *
 * 封装 InprocQueue 和 IpcQueue，对外提供统一的消息发送/订阅接口。
 * 使用 Builder 模式替代 Two-Phase Initialization。
 * 内部自动构建 MessageHeader 并填充时间戳和序列号。
 */
class MessageManager {
public:
    /** 消息回调函数类型 */
    using MessageCallback = std::function<void(const MessageHeader&, const std::string&)>;

    /**
     * Builder 内部类
     *
     * 提供链式配置接口，通过 build() 创建 MessageManager 实例。
     * 所有配置项都有合理默认值，只需设置必要的端点即可。
     */
    class Builder {
    public:
        /** 设置进程内端点 */
        Builder& inprocEndpoint(const std::string& ep) {
            config_.inprocEndpoint = ep; return *this;
        }
        /** 设置进程间端点（为空则不启用 IPC） */
        Builder& ipcEndpoint(const std::string& ep) {
            config_.ipcEndpoint = ep; return *this;
        }
        /** 设置 IPC 为 PAIR 服务端模式（bind 端点，一对一） */
        Builder& asPairServer() {
            config_.ipcMode = IpcMode::PairServer; return *this;
        }
        /** 设置 IPC 为 PAIR 客户端模式（connect 到服务端，一对一） */
        Builder& asPairClient() {
            config_.ipcMode = IpcMode::PairClient; return *this;
        }
        /** 设置 IPC 为 PUB 发布端模式（bind 端点，一对多广播） */
        Builder& asPublisher() {
            config_.ipcMode = IpcMode::Publisher; return *this;
        }
        /** 设置 IPC 为 SUB 订阅端模式（connect 到发布端，接收消息） */
        Builder& asSubscriber() {
            config_.ipcMode = IpcMode::Subscriber; return *this;
        }
        /** 设置 IPC 为 ROUTER 模式（中心节点，多对多双向） */
        Builder& asRouter(const std::string& name) {
            config_.ipcMode = IpcMode::Router;
            config_.peerName = name;
            return *this;
        }
        /** 设置 IPC 为 DEALER 模式（边缘节点，多对多双向） */
        Builder& asDealer(const std::string& name) {
            config_.ipcMode = IpcMode::Dealer;
            config_.peerName = name;
            return *this;
        }
        /** 设置发送超时（毫秒） */
        Builder& sendTimeout(int ms) {
            config_.sendTimeoutMs = ms; return *this;
        }
        /** 设置接收超时（毫秒） */
        Builder& recvTimeout(int ms) {
            config_.recvTimeoutMs = ms; return *this;
        }
        /** 设置发送高水位 */
        Builder& sendHwm(int hwm) {
            config_.sendHwm = hwm; return *this;
        }
        /** 设置接收高水位 */
        Builder& recvHwm(int hwm) {
            config_.recvHwm = hwm; return *this;
        }
        /** 设置日志回调函数 */
        Builder& logCallback(MqConfig::LogCallback cb) {
            config_.logCallback = std::move(cb); return *this;
        }

        /**
         * 构建 MessageManager 实例
         *
         * 根据配置创建并初始化内部队列。
         * 如果 inprocEndpoint 为空或初始化失败，返回 nullptr。
         *
         * @return MessageManager 智能指针，失败返回 nullptr
         */
        std::unique_ptr<MessageManager> build();

    private:
        MqConfig config_;
    };

    /**
     * 创建 Builder
     *
     * 静态工厂方法，用于启动链式配置。
     *
     * @return Builder 实例
     */
    static Builder builder() { return Builder{}; }

    /**
     * 析构函数
     *
     * 自动调用 stop() 停止所有消息队列并释放资源。
     */
    ~MessageManager();

    /**
     * 启动消息管理器
     *
     * 依次启动进程内队列和进程间队列（如果有），启动各自的接收线程。
     * 必须在 build() 成功后调用。
     *
     * @return MqResult 操作结果
     */
    MqResult start();

    /**
     * 停止消息管理器
     *
     * 取消所有订阅，停止所有消息队列，关闭接收线程和 socket。
     * 这是完整的清理方法，析构函数会自动调用。
     */
    void stop();

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
    MqResult send(InprocMessageType type, const std::string& payload = "");

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
    MqResult send(IpcMessageType type, const std::string& payload = "");

    /**
     * 发送进程内消息（通用接口，支持自定义类型）
     *
     * 允许应用使用自定义消息类型（10000-65535 范围）。
     * 自动构建 MessageHeader 并填充 INPROC 域、消息类型、时间戳和序列号。
     *
     * 使用示例：
     *   namespace my_app {
     *       enum class CustomType : uint32_t {
     *           MOTION_DETECTED = 10000,
     *       };
     *   }
     *   manager->send(static_cast<uint32_t>(my_app::CustomType::MOTION_DETECTED), payload);
     *
     * @param type 消息类型（可以是库内置或应用自定义类型）
     * @param payload 消息负载（JSON 格式字符串），默认为空
     * @return MqResult 操作结果
     */
    MqResult send(uint32_t type, const std::string& payload = "");

    /**
     * 发送进程间消息（通用接口，支持自定义类型）
     *
     * 允许应用使用自定义消息类型（10000-65535 范围）。
     * 自动构建 MessageHeader 并填充 IPC 域、消息类型、时间戳和序列号。
     * 如果未初始化 IPC 队列，则返回失败。
     *
     * @param type 消息类型（可以是库内置或应用自定义类型）
     * @param payload 消息负载（JSON 格式字符串），默认为空
     * @return MqResult 操作结果
     */
    MqResult sendIpc(uint32_t type, const std::string& payload = "");

    /**
     * 发送消息给指定目标（ROUTER 模式用）
     *
     * ROUTER 模式下，根据目标 peer 名字查找其 ZMQ 身份标识，然后发送。
     * 自动填充 from 字段为本节点名字。
     *
     * @param targetName 目标 peer 名字（如 "ota_agent"）
     * @param type 消息类型
     * @param payload 消息负载
     * @return MqResult 操作结果
     */
    MqResult sendTo(const std::string& targetName, uint32_t type, const std::string& payload = "");

    /**
     * 发送消息给指定目标（ROUTER 模式用，库内置消息类型）
     */
    MqResult sendTo(const std::string& targetName, IpcMessageType type, const std::string& payload = "");

    /**
     * 订阅进程内消息
     *
     * 在 InprocQueue 中注册指定类型消息的回调函数。
     * 回调会一直有效,直到调用 unsubscribe() 或 clearAll()。
     *
     * @param type 要订阅的消息类型
     * @param callback 消息到达时的回调函数
     */
    void subscribe(InprocMessageType type, MessageCallback callback);

    /**
     * 订阅进程间消息
     *
     * 在 IpcQueue 中注册指定类型消息的回调函数。
     * 如果未初始化 IPC 队列,则忽略。
     *
     * @param type 要订阅的消息类型
     * @param callback 消息到达时的回调函数
     */
    void subscribe(IpcMessageType type, MessageCallback callback);

    /**
     * 订阅进程内消息（通用接口，支持自定义类型）
     *
     * 允许应用订阅自定义消息类型（10000-65535 范围）。
     *
     * @param type 要订阅的消息类型（可以是库内置或应用自定义类型）
     * @param callback 消息到达时的回调函数
     */
    void subscribe(uint32_t type, MessageCallback callback);

    /**
     * 订阅进程间消息（通用接口，支持自定义类型）
     *
     * 允许应用订阅自定义消息类型（10000-65535 范围）。
     *
     * @param type 要订阅的消息类型（可以是库内置或应用自定义类型）
     * @param callback 消息到达时的回调函数
     */
    void subscribeIpc(uint32_t type, MessageCallback callback);

    /**
     * 取消所有订阅
     *
     * 清空 InprocQueue 和 IpcQueue 中所有已注册的回调函数。
     * 通常在程序退出时调用。
     */
    void clearAll();

    /** 检查消息管理器是否已初始化 */
    bool isInitialized() const { return initialized_; }

    /** 检查消息管理器是否正在运行 */
    bool isRunning() const { return running_; }

private:
    /**
     * 私有构造函数
     *
     * 只能通过 Builder::build() 创建实例。
     *
     * @param config 消息队列配置
     */
    explicit MessageManager(const MqConfig& config);

    MqConfig config_;

    std::unique_ptr<InprocQueue> inprocQueue_;  // 进程内消息队列（必选）
    std::unique_ptr<IpcQueue> ipcQueue_;        // 进程间消息队列（可选）

    bool initialized_ = false;                  // 初始化标志
    bool running_ = false;                      // 运行标志
    uint32_t sequence_ = 0;                     // 全局消息序列号（单调递增）
};

}
