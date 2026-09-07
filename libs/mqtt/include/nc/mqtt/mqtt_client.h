/**
 * mqtt_client.h
 * MQTT客户端头文件 - 自愈架构版本
 *
 * 迁自 gw_av100/application/iot_live（已上线验证），迁入改动仅三处：
 *   1) 包裹 nc::mqtt 命名空间（与 nc::rtc 等库惯例一致）
 *   2) 日志换 NC_LOGx 门面（原 fmt 风格 LOG_xxx）
 *   3) 删除 onConnectFailure 里的 system("nc -z") 网络探测
 *      （诊断用途，回调路径 fork shell 不适合常驻进程）及其配置项
 *
 * 架构设计：
 * - 底层使用 Paho MQTTAsync 异步 API：connect/publish/subscribe 立即返回，结果由回调通知
 * - 自动重连：使用 Paho 内置 automaticReconnect（指数退避，由 minRetryInterval/maxRetryInterval 控制）
 * - 防扎堆：仅在首次连接前根据 clientId 哈希注入一次随机延迟，避免设备群同时上线
 * - 消息缓冲：断线期间将 publish 入队（最多 100 条），重连成功后由代码层重发
 * - 订阅恢复：重连成功后由代码层批量恢复订阅
 * - 业务回调线程：将到达消息从 Paho 内部线程转移到独立线程派发，避免阻塞 Paho 网络线程
 */

#ifndef NC_MQTT_CLIENT_H
#define NC_MQTT_CLIENT_H

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <vector>
#include <deque>

// paho-mqtt 头文件（异步 API，include 路径由 nc::mqtt 传播）
#include "MQTTAsync.h"

namespace nc {
namespace mqtt {

/**
 * MQTT连接状态枚举
 */
enum class MqttState {
    Disconnected,   // 已断开
    Connecting,     // 正在连接
    Connected,      // 已连接
    Reconnecting    // 正在重连
};

/**
 * MQTT配置结构体
 */
struct MqttConfig {
    std::string address;                    // 服务器地址（如 tcp://192.168.1.100:1883）
    std::string clientId;                   // 客户端唯一标识
    std::string username;                   // 用户名（ThingsBoard的accessToken）
    std::string password;                   // 密码
    int keepAliveInterval = 60;             // 心跳间隔（秒），默认60秒
    bool autoReconnect = true;              // 是否启用自动重连
    int maxReconnectAttempts = 0;           // 最大重连次数（0表示无限重试）
    int initialReconnectIntervalMs = 2000;  // 初始重连间隔（毫秒）
    int maxReconnectIntervalMs = 30000;     // 最大重连间隔（毫秒）
    int connectTimeoutMs = 10000;           // 连接超时时间（毫秒）
    int defaultQos = 1;                     // 默认QoS等级
    std::vector<std::string> topics;        // 订阅主题列表，连接成功后自动订阅，重连后自动恢复（使用defaultQos）
    bool enableAntiThundering = true;       // 是否启用防扎堆机制（避免大量设备同时连接服务器）
    int maxRandomJitterMs = 5000;           // 最大随机抖动时间（毫秒），默认5秒

    // SSL/TLS 配置
    bool enableSsl = false;                   // 是否启用 SSL/TLS（可根据地址前缀自动推断）
    bool verifyServerCert = true;             // 是否验证服务器证书链
};

/**
 * MqttClient类
 * 自愈型MQTT客户端，支持自动重连、消息缓冲、订阅恢复
 */
class MqttClient {
public:
    /** 消息到达回调函数类型 */
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    /** 状态变化回调函数类型 */
    using StateCallback = std::function<void(MqttState state, const std::string& info)>;

    /**
     * 构造函数
     * @param config MQTT配置
     */
    explicit MqttClient(MqttConfig config);

    /**
     * 析构函数
     * 自动停止客户端并释放资源
     */
    ~MqttClient();

    // 禁止拷贝和移动（单例模式最佳实践）
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    /**
     * 启动MQTT客户端（异步非阻塞）
     * 启动独立的重连管理线程，由线程负责连接和自动重连
     * @return 是否启动成功
     */
    bool start();

    /**
     * 停止MQTT客户端
     * 通知重连线程退出，等待线程结束，断开连接
     */
    void stop();

    /**
     * 发布消息（非阻塞）
     * 如果已连接，立即发送；如果断线且启用自动重连，则缓存到队列
     * @param topic 主题
     * @param payload 消息内容
     * @param qos 服务质量等级(0/1/2)
     * @param retained 是否保留消息
     * @return 1=已发送到服务器, 2=已缓存到队列, 0=失败
     */
    int publish(const std::string& topic, const std::string& payload, int qos = 1, bool retained = false);

    /**
     * 订阅主题
     * 如果已连接，立即订阅；无论连接状态，都保存订阅信息（用于重连后恢复）
     * @param topic 主题
     * @param qos 服务质量等级
     * @return 是否订阅成功或保存成功
     */
    bool subscribe(const std::string& topic, int qos = 1);

    /**
     * 取消订阅
     * @param topic 主题
     * @return 是否取消成功
     */
    bool unsubscribe(const std::string& topic);

    /**
     * 设置消息到达回调
     * @param callback 回调函数
     */
    void setMessageCallback(MessageCallback callback);

    /**
     * 设置状态变化回调
     * @param callback 回调函数
     */
    void setStateCallback(StateCallback callback);

    /**
     * 获取当前状态
     * @return MQTT连接状态
     */
    MqttState getState() const;

    /**
     * 检查是否已连接
     * @return 是否已连接
     */
    bool isConnected() const;

private:
    // ========== Paho 异步库回调（静态函数） ==========

    /**
     * 消息到达回调（由 Paho 库调用）
     * 注意：异步签名与同步版本相同，但 message 类型为 MQTTAsync_message*
     */
    static int onMessageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message);

    /**
     * 连接丢失回调（由 Paho 库调用）
     * 启用 automaticReconnect 时，Paho 会在该回调后自动开始重连
     */
    static void onConnectionLost(void* context, char* cause);

    /**
     * 自动重连成功回调（由 Paho 库调用）
     * 通过 MQTTAsync_setConnected 注册，每次（含首次后的）重连成功都会触发
     */
    static void onConnected(void* context, char* cause);

    /**
     * 首次 connect 成功回调
     */
    static void onConnectSuccess(void* context, MQTTAsync_successData* response);

    /**
     * 首次 connect 失败回调
     * 认证失败（rc=4/5）时主动停止重连
     */
    static void onConnectFailure(void* context, MQTTAsync_failureData* response);

    /**
     * 发布失败回调
     */
    static void onPublishFailure(void* context, MQTTAsync_failureData* response);

    /**
     * 订阅失败回调
     */
    static void onSubscribeFailure(void* context, MQTTAsync_failureData* response);

    // ========== 内部实现函数 ==========

    /**
     * 状态变化处理
     * 更新状态并通知业务层
     * @param newState 新状态
     * @param info 状态描述信息
     */
    void onStateChanged(MqttState newState, const std::string& info);

    /**
     * 重发消息缓冲
     * 重连成功后，将断线期间缓存的消息重新发送
     */
    void retryPendingMessages();

    /**
     * 恢复所有订阅
     * 重连成功后，重新订阅所有之前订阅过的 topic
     */
    void resubscribeAll();

    /**
     * 首次连接前的防扎堆随机延迟
     * 基于 clientId 哈希作为随机种子，使设备群上线时间错开
     */
    void applyAntiThunderingDelay();

    /**
     * 连接成功后的统一恢复动作（订阅恢复 + 缓冲重发 + 状态通知）
     */
    void handleConnected(const std::string& info);

    // ========== 成员变量 ==========

    MqttConfig config_;                     // MQTT配置
    MQTTAsync client_ = nullptr;            // Paho MQTT 异步客户端句柄
    MQTTAsync_SSLOptions sslOpts_;          // SSL 选项（必须为成员变量，避免局部变量生命周期问题）

    // 状态管理
    std::atomic<MqttState> state_{MqttState::Disconnected};  // 当前连接状态（原子操作）
    std::mutex stateMutex_;                 // 状态互斥锁

    // 线程控制
    std::atomic<bool> shouldStop_{false};   // 线程退出标志
    std::atomic<bool> authFailed_{false};   // 认证失败标志（停止后续自动重连）

    // 回调函数
    MessageCallback messageCallback_;       // 消息到达回调
    StateCallback stateCallback_;           // 状态变化回调
    std::mutex callbackMutex_;              // 回调互斥锁（保护回调函数）

    // 订阅列表（重连后重新订阅）
    std::vector<std::pair<std::string, int>> subscriptions_;  // 订阅列表（topic, qos）
    std::mutex subscriptionsMutex_;         // 订阅列表互斥锁

    // 消息缓冲（断开时缓存）
    struct PendingMessage {
        std::string topic;
        std::string payload;
        int qos;
        bool retained;
    };
    std::deque<PendingMessage> pendingMessages_;      // 待发送消息队列
    std::mutex pendingMutex_;                         // 消息队列互斥锁
    static constexpr size_t MAX_PENDING_MESSAGES = 100;   // 最大缓冲消息数

    // 异步回调处理队列（避免阻塞Paho网络线程）
    enum class CallbackType {
        Message,   // messageArrived
        State      // stateChanged
    };
    struct CallbackMessage {
        CallbackType type{CallbackType::Message};
        // Message 类型字段
        std::string topic;
        std::string payload;
        // State 类型字段
        MqttState state{MqttState::Disconnected};
        std::string info;
    };
    std::deque<CallbackMessage> callbackQueue_;       // 回调消息队列
    std::mutex callbackQueueMutex_;                   // 回调队列互斥锁
    std::condition_variable callbackCV_;              // 回调线程条件变量
    std::thread callbackThread_;                      // 回调处理线程
    static constexpr size_t MAX_CALLBACK_QUEUE = 5000;  // 回调队列最大容量
    void callbackThreadFunc();                        // 回调处理线程函数
};

} // namespace mqtt
} // namespace nc

#endif // NC_MQTT_CLIENT_H
