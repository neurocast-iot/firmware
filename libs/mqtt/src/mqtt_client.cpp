/**
 * mqtt_client.cpp
 * MQTT 客户端实现 - 异步 API 版本（Paho MQTTAsync）
 *
 * 迁自 gw_av100/application/iot_live（已上线验证），迁入改动见 mqtt_client.h 头注释
 *
 * 架构设计：
 * - 底层使用 Paho MQTTAsync 异步 API：connect/publish/subscribe 立即返回，结果由回调通知
 * - 自动重连：使用 Paho 内置 automaticReconnect（指数退避，由 minRetryInterval/maxRetryInterval 控制）
 * - 防扎堆：仅在首次连接前根据 clientId 哈希注入一次随机延迟，避免设备群同时上线
 * - 消息缓冲：断线期间将 publish 入队（最多 100 条），重连成功后由代码层重发
 * - 订阅恢复：重连成功后由代码层批量恢复订阅
 * - 业务回调线程：将到达消息从 Paho 内部线程转移到独立线程派发，避免阻塞 Paho 网络线程
 */

#include "nc/mqtt/mqtt_client.h"
#include "nc/common/log_utils.h"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <random>

namespace nc {
namespace mqtt {

// static constexpr 成员变量的类外定义（C++14 需要）
constexpr size_t MqttClient::MAX_PENDING_MESSAGES;
constexpr size_t MqttClient::MAX_CALLBACK_QUEUE;

/**
 * 构造函数
 * 创建 MQTTAsync 客户端实例并注册基础回调
 */
MqttClient::MqttClient(MqttConfig config)
    : config_(std::move(config)) {
    // 创建底层 Paho 异步客户端
    int rc = MQTTAsync_create(&client_, config_.address.c_str(), config_.clientId.c_str(),
                              MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTASYNC_SUCCESS) {
        NC_LOGE("[MQTT] MQTTAsync_create 失败: rc={}", rc);
    }

    // 注册基础回调：连接丢失、消息到达（QoS1/2 投递完成回调本实现不需要）
    MQTTAsync_setCallbacks(client_, this, onConnectionLost, onMessageArrived, NULL);

    // 注册自动重连成功回调（每次重连成功都会触发；首次连接成功也会触发）
    MQTTAsync_setConnected(client_, this, onConnected);

    // 初始化订阅列表（从配置复制，用于重连后自动恢复）
    {
        std::lock_guard<std::mutex> lock(subscriptionsMutex_);
        for (const auto& topic : config_.topics) {
            subscriptions_.emplace_back(topic, config_.defaultQos);
        }
    }

    NC_LOGI("[MQTT] 客户端创建成功: address={}, clientId={}, 预设订阅数={}",
            config_.address.c_str(), config_.clientId.c_str(), config_.topics.size());
}

/**
 * 析构函数
 * 停止客户端并销毁底层句柄
 */
MqttClient::~MqttClient() {
    stop();
    if (client_) {
        MQTTAsync_destroy(&client_);
        client_ = nullptr;
    }
    NC_LOGI("[MQTT] 客户端已销毁");
}

/**
 * 启动 MQTT 客户端（异步非阻塞）
 * 启动业务回调线程，应用首连防扎堆延迟，发起异步连接
 */
bool MqttClient::start() {
    MqttState currentState = state_.load();
    if (currentState != MqttState::Disconnected) {
        NC_LOGW("[MQTT] 客户端已启动，当前状态={}", static_cast<int>(currentState));
        return false;
    }

    NC_LOGI("[MQTT] 正在启动客户端...");
    shouldStop_.store(false);
    authFailed_.store(false);

    // 启动业务回调处理线程（解耦业务回调与 Paho 网络线程）
    callbackThread_ = std::thread(&MqttClient::callbackThreadFunc, this);

    // 防扎堆：基于 clientId 哈希的一次性随机延迟
    applyAntiThunderingDelay();

    // 配置异步连接选项
    MQTTAsync_connectOptions connOpts = MQTTAsync_connectOptions_initializer;
    connOpts.keepAliveInterval = config_.keepAliveInterval;
    connOpts.cleansession = 1;
    connOpts.connectTimeout = std::max(1, config_.connectTimeoutMs / 1000);

    // Paho 内置自动重连（指数退避：min → 翻倍 → max）
    connOpts.automaticReconnect = config_.autoReconnect ? 1 : 0;
    connOpts.minRetryInterval = std::max(1, config_.initialReconnectIntervalMs / 1000);
    connOpts.maxRetryInterval = std::max(connOpts.minRetryInterval, config_.maxReconnectIntervalMs / 1000);

    // 认证信息
    if (!config_.username.empty()) {
        connOpts.username = config_.username.c_str();
    }
    if (!config_.password.empty()) {
        connOpts.password = config_.password.c_str();
    }

    // 首次连接结果回调
    connOpts.onSuccess = onConnectSuccess;
    connOpts.onFailure = onConnectFailure;
    connOpts.context = this;

    // 配置 SSL/TLS 选项（如果启用）
    if (config_.enableSsl) {
        sslOpts_ = MQTTAsync_SSLOptions_initializer;
        sslOpts_.enableServerCertAuth = config_.verifyServerCert ? 1 : 0;
        connOpts.ssl = &sslOpts_;
        NC_LOGI("[MQTT] SSL/TLS 已启用, 证书验证={}",
                config_.verifyServerCert ? "开启" : "关闭");
    }

    state_.store(MqttState::Connecting);
    NC_LOGI("[MQTT] 发起异步连接: address={}, autoReconnect={}, minRetry={}s, maxRetry={}s",
            config_.address.c_str(), connOpts.automaticReconnect,
            connOpts.minRetryInterval, connOpts.maxRetryInterval);

    int rc = MQTTAsync_connect(client_, &connOpts);
    if (rc != MQTTASYNC_SUCCESS) {
        NC_LOGE("[MQTT] MQTTAsync_connect 启动失败: rc={}", rc);
        state_.store(MqttState::Disconnected);
        return false;
    }

    return true;
}

/**
 * 停止 MQTT 客户端
 * 通知回调线程退出，发起异步断开，等待线程结束
 */
void MqttClient::stop() {
    NC_LOGI("[MQTT] 正在停止客户端...");

    shouldStop_.store(true);
    callbackCV_.notify_all();

    // 异步断开连接
    if (client_ && MQTTAsync_isConnected(client_)) {
        MQTTAsync_disconnectOptions discOpts = MQTTAsync_disconnectOptions_initializer;
        discOpts.timeout = 5000;  // ms
        int rc = MQTTAsync_disconnect(client_, &discOpts);
        if (rc != MQTTASYNC_SUCCESS) {
            NC_LOGW("[MQTT] MQTTAsync_disconnect 启动失败: rc={}", rc);
        }
    }

    // 等待回调处理线程结束
    if (callbackThread_.joinable()) {
        callbackThread_.join();
    }

    state_.store(MqttState::Disconnected);
    NC_LOGI("[MQTT] 客户端已停止");
}

/**
 * 发布消息（非阻塞）
 * 已连接：调用 MQTTAsync_sendMessage 立即返回；
 * 未连接且启用自动重连：缓存到队列；
 */
int MqttClient::publish(const std::string& topic, const std::string& payload, int qos, bool retained) {
    MqttState currentState = state_.load();

    if (currentState == MqttState::Connected && client_ && MQTTAsync_isConnected(client_)) {
        NC_LOGD("[MQTT] 尝试发布消息: topic={}, payload_len={}", topic.c_str(), payload.length());

        MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
        pubmsg.payload = const_cast<char*>(payload.c_str());
        pubmsg.payloadlen = static_cast<int>(payload.length());
        pubmsg.qos = qos;
        pubmsg.retained = retained ? 1 : 0;

        MQTTAsync_responseOptions respOpts = MQTTAsync_responseOptions_initializer;
        respOpts.context = this;
        respOpts.onFailure = onPublishFailure;  // 仅关注失败；成功默认无操作

        int rc = MQTTAsync_sendMessage(client_, topic.c_str(), &pubmsg, &respOpts);

        if (rc == MQTTASYNC_SUCCESS) {
            NC_LOGD("[MQTT] 发布已提交: topic={}", topic.c_str());
            return 1;  // 已提交到 Paho 内部队列
        }
        NC_LOGE("[MQTT] 发布失败: rc={}, topic={}", rc, topic.c_str());
    }

    // 未连接但启用自动重连：缓存到队列
    if (config_.autoReconnect) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pendingMessages_.size() >= MAX_PENDING_MESSAGES) {
            NC_LOGW("[MQTT] 缓冲队列已满，丢弃最旧消息");
            pendingMessages_.pop_front();
        }
        pendingMessages_.push_back({topic, payload, qos, retained});
        NC_LOGI("[MQTT] 消息已缓存，队列大小={}", pendingMessages_.size());
        return 2;
    }

    NC_LOGE("[MQTT] 发布失败: 未连接且未启用自动重连");
    return 0;
}

/**
 * 订阅主题
 * 已连接：立即向服务器发起异步订阅；
 * 无论连接状态都保存订阅信息（用于重连后批量恢复）。
 */
bool MqttClient::subscribe(const std::string& topic, int qos) {
    if (state_.load() == MqttState::Connected && client_) {
        MQTTAsync_responseOptions respOpts = MQTTAsync_responseOptions_initializer;
        respOpts.context = this;
        respOpts.onFailure = onSubscribeFailure;

        int rc = MQTTAsync_subscribe(client_, topic.c_str(), qos, &respOpts);
        if (rc == MQTTASYNC_SUCCESS) {
            NC_LOGI("[MQTT] 订阅已提交: topic={}, qos={}", topic.c_str(), qos);
        } else {
            NC_LOGE("[MQTT] 订阅启动失败: rc={}, topic={}", rc, topic.c_str());
            return false;
        }
    }

    // 保存订阅信息（重连后自动恢复）
    {
        std::lock_guard<std::mutex> lock(subscriptionsMutex_);
        for (const auto& sub : subscriptions_) {
            if (sub.first == topic) {
                NC_LOGI("[MQTT] 订阅已存在: topic={}", topic.c_str());
                return true;
            }
        }
        subscriptions_.emplace_back(topic, qos);
    }

    NC_LOGI("[MQTT] 订阅信息已保存: topic={}, qos={}", topic.c_str(), qos);
    return true;
}

/**
 * 取消订阅
 */
bool MqttClient::unsubscribe(const std::string& topic) {
    if (state_.load() == MqttState::Connected && client_) {
        MQTTAsync_responseOptions respOpts = MQTTAsync_responseOptions_initializer;
        respOpts.context = this;

        int rc = MQTTAsync_unsubscribe(client_, topic.c_str(), &respOpts);
        if (rc != MQTTASYNC_SUCCESS) {
            NC_LOGE("[MQTT] 取消订阅启动失败: rc={}, topic={}", rc, topic.c_str());
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(subscriptionsMutex_);
        subscriptions_.erase(
            std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                [&topic](const std::pair<std::string, int>& sub) {
                    return sub.first == topic;
                }),
            subscriptions_.end()
        );
    }

    NC_LOGI("[MQTT] 已取消订阅: topic={}", topic.c_str());
    return true;
}

void MqttClient::setMessageCallback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    messageCallback_ = std::move(callback);
}

void MqttClient::setStateCallback(StateCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    stateCallback_ = std::move(callback);
}

MqttState MqttClient::getState() const {
    return state_.load();
}

bool MqttClient::isConnected() const {
    return state_.load() == MqttState::Connected;
}

/**
 * 业务回调处理线程
 * 从 callbackQueue_ 取消息，调用业务侧 messageCallback_
 * 与 Paho 网络线程解耦，避免业务回调阻塞库内部
 */
void MqttClient::callbackThreadFunc() {
    NC_LOGI("[MQTT] 回调处理线程已启动");

    while (!shouldStop_.load()) {
        CallbackMessage msg;
        {
            std::unique_lock<std::mutex> lock(callbackQueueMutex_);
            callbackCV_.wait(lock, [this]() {
                return shouldStop_.load() || !callbackQueue_.empty();
            });

            if (shouldStop_.load() && callbackQueue_.empty()) {
                break;
            }

            if (!callbackQueue_.empty()) {
                msg = std::move(callbackQueue_.front());
                callbackQueue_.pop_front();
            } else {
                continue;
            }
        }

        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (msg.type == CallbackType::Message) {
            if (messageCallback_) {
                try {
                    messageCallback_(msg.topic, msg.payload);
                } catch (const std::exception& e) {
                    NC_LOGE("[MQTT] 消息回调异常: {}", e.what());
                }
            }
        } else { // CallbackType::State
            if (stateCallback_) {
                try {
                    stateCallback_(msg.state, msg.info);
                } catch (const std::exception& e) {
                    NC_LOGE("[MQTT] 状态回调异常: {}", e.what());
                }
            }
        }
    }

    NC_LOGI("[MQTT] 回调处理线程已退出");
}

/**
 * 状态变化处理：更新原子状态并入队，由 callbackThread 异步派发给业务层
 * 不在当前线程同步调用 stateCallback_，避免：
 *   1) 业务回调中调 MQTTAsync_* API 与 Paho 内部锁形成递归/死锁；
 *   2) 业务回调耗时过长阻塞 Paho 网络线程。
 */
void MqttClient::onStateChanged(MqttState newState, const std::string& info) {
    NC_LOGI("[MQTT] 状态变化: {} -> {}, 信息={}",
            static_cast<int>(state_.load()), static_cast<int>(newState), info.c_str());

    state_.store(newState);

    {
        std::lock_guard<std::mutex> lock(callbackQueueMutex_);
        if (callbackQueue_.size() >= MAX_CALLBACK_QUEUE) {
            NC_LOGW("[MQTT] 回调队列已满({})，丢弃最旧消息", MAX_CALLBACK_QUEUE);
            callbackQueue_.pop_front();
        }
        CallbackMessage msg;
        msg.type = CallbackType::State;
        msg.state = newState;
        msg.info = info;
        callbackQueue_.push_back(std::move(msg));
    }
    callbackCV_.notify_one();
}

/**
 * 重发消息缓冲
 */
void MqttClient::retryPendingMessages() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pendingMessages_.empty()) {
        return;
    }

    NC_LOGI("[MQTT] 正在重发{}条缓存消息", pendingMessages_.size());

    auto it = pendingMessages_.begin();
    while (it != pendingMessages_.end()) {
        MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
        pubmsg.payload = const_cast<char*>(it->payload.c_str());
        pubmsg.payloadlen = static_cast<int>(it->payload.length());
        pubmsg.qos = it->qos;
        pubmsg.retained = it->retained ? 1 : 0;

        MQTTAsync_responseOptions respOpts = MQTTAsync_responseOptions_initializer;
        respOpts.context = this;
        respOpts.onFailure = onPublishFailure;

        int rc = MQTTAsync_sendMessage(client_, it->topic.c_str(), &pubmsg, &respOpts);
        if (rc == MQTTASYNC_SUCCESS) {
            NC_LOGI("[MQTT] 缓存消息已提交: topic={}", it->topic.c_str());
            it = pendingMessages_.erase(it);
        } else {
            NC_LOGE("[MQTT] 缓存消息提交失败: rc={}, topic={}", rc, it->topic.c_str());
            ++it;
        }
    }
}

/**
 * 恢复所有订阅
 */
void MqttClient::resubscribeAll() {
    std::lock_guard<std::mutex> lock(subscriptionsMutex_);
    if (subscriptions_.empty()) {
        return;
    }

    NC_LOGI("[MQTT] 正在恢复{}个订阅", subscriptions_.size());

    for (const auto& sub : subscriptions_) {
        MQTTAsync_responseOptions respOpts = MQTTAsync_responseOptions_initializer;
        respOpts.context = this;
        respOpts.onFailure = onSubscribeFailure;

        int rc = MQTTAsync_subscribe(client_, sub.first.c_str(), sub.second, &respOpts);
        if (rc == MQTTASYNC_SUCCESS) {
            NC_LOGI("[MQTT] 订阅已恢复: topic={}", sub.first.c_str());
        } else {
            NC_LOGE("[MQTT] 订阅恢复失败: rc={}, topic={}", rc, sub.first.c_str());
        }
    }
}

/**
 * 首连防扎堆延迟
 * 仅在 start() 流程被调用一次；不参与后续重连（重连由 Paho 处理）
 * 迁入改动：srand/rand 换局部 minstd_rand，不污染进程全局随机数状态
 */
void MqttClient::applyAntiThunderingDelay() {
    if (!config_.enableAntiThundering || config_.maxRandomJitterMs <= 0) {
        return;
    }
    unsigned int hash = 0;
    for (char c : config_.clientId) {
        hash = hash * 31 + static_cast<unsigned char>(c);
    }
    std::minstd_rand rng(hash + static_cast<unsigned int>(time(NULL)));
    int jitter = static_cast<int>(rng() % static_cast<unsigned int>(config_.maxRandomJitterMs));
    NC_LOGI("[MQTT] 首次连接防扎堆：延迟 {}ms", jitter);
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter));
}

/**
 * 连接成功后的统一恢复动作
 */
void MqttClient::handleConnected(const std::string& info) {
    onStateChanged(MqttState::Connected, info);
    resubscribeAll();
    retryPendingMessages();
}

// ========================== Paho 静态回调 ==========================

/**
 * 消息到达回调
 * 快速将消息转入异步队列，避免阻塞 Paho 网络线程
 */
int MqttClient::onMessageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message) {
    (void)topicLen;
    MqttClient* client = static_cast<MqttClient*>(context);
    if (!client) {
        return 1;
    }

    std::string topic(topicName);
    std::string payload(static_cast<char*>(message->payload), message->payloadlen);

    {
        std::lock_guard<std::mutex> lock(client->callbackQueueMutex_);
        if (client->callbackQueue_.size() >= MAX_CALLBACK_QUEUE) {
            NC_LOGW("[MQTT] 回调队列已满({})，丢弃最旧消息", MAX_CALLBACK_QUEUE);
            client->callbackQueue_.pop_front();
        }
        client->callbackQueue_.push_back({CallbackType::Message, std::move(topic), std::move(payload), MqttState::Disconnected, std::string()});
    }
    client->callbackCV_.notify_one();

    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topicName);
    return 1;
}

/**
 * 连接丢失回调
 * 启用 automaticReconnect 时，Paho 会在该回调返回后自动开始重试
 * 此处仅更新状态、通知业务层，不主动发起重连
 */
void MqttClient::onConnectionLost(void* context, char* cause) {
    MqttClient* client = static_cast<MqttClient*>(context);
    if (!client) {
        return;
    }

    std::string causeStr(cause ? cause : "Unknown");
    NC_LOGE("[MQTT] 连接断开: {}", causeStr.c_str());
    client->onStateChanged(MqttState::Reconnecting, "连接断开: " + causeStr);
}

/**
 * 自动重连成功回调（含首次连接成功）
 * 通过 MQTTAsync_setConnected 注册；Paho 在每次重连完成后会触发该回调
 */
void MqttClient::onConnected(void* context, char* cause) {
    MqttClient* client = static_cast<MqttClient*>(context);
    if (!client) {
        return;
    }
    std::string causeStr(cause ? cause : "");
    NC_LOGI("[MQTT] 已（重新）连接到服务器: {}", causeStr.c_str());
    client->handleConnected("连接成功" + (causeStr.empty() ? "" : (": " + causeStr)));
}

/**
 * 首次 connect 成功回调
 * 注：onConnected 也会同步触发；本回调仅做日志，避免重复执行恢复动作
 */
void MqttClient::onConnectSuccess(void* context, MQTTAsync_successData* response) {
    (void)response;
    MqttClient* client = static_cast<MqttClient*>(context);
    if (!client) {
        return;
    }
    NC_LOGI("[MQTT] 首次连接成功（onSuccess）");
    // 实际状态/订阅恢复由 onConnected 统一处理
}

/**
 * 首次 connect 失败回调
 * 注意：启用 automaticReconnect 后，Paho 仍会自动重试；
 * 但若是认证失败（rc=4/5），重试无意义，主动断开并标记失败。
 * 迁入改动：删除原实现的 system("nc -z") 网络探测（诊断用途，
 * 回调路径 fork shell 不适合常驻进程；可达性由 Paho 重连结果直接体现）
 */
void MqttClient::onConnectFailure(void* context, MQTTAsync_failureData* response) {
    MqttClient* client = static_cast<MqttClient*>(context);
    if (!client) {
        return;
    }

    int code = response ? response->code : -1;
    const char* msg = (response && response->message) ? response->message : "";
    NC_LOGE("[MQTT] 首次连接失败: rc={}, msg={}", code, msg);

    // Paho MQTT 错误码：4=认证失败（用户名/密码错误），5=未授权
    if (code == 4 || code == 5) {
        NC_LOGE("[MQTT] 认证失败，停止后续重连");
        client->authFailed_.store(true);

        // 主动断开以阻止 Paho 后续自动重试
        if (client->client_) {
            MQTTAsync_disconnectOptions discOpts = MQTTAsync_disconnectOptions_initializer;
            discOpts.timeout = 0;
            MQTTAsync_disconnect(client->client_, &discOpts);
        }

        client->onStateChanged(MqttState::Disconnected, "认证失败，已停止重连");
        return;
    }

    // 其它错误码：状态切到 Reconnecting，由 Paho automaticReconnect 接管
    client->onStateChanged(MqttState::Reconnecting, std::string("首次连接失败，等待自动重连: ") + msg);
}

/**
 * 发布失败回调
 */
void MqttClient::onPublishFailure(void* context, MQTTAsync_failureData* response) {
    (void)context;
    int code = response ? response->code : -1;
    const char* msg = (response && response->message) ? response->message : "";
    NC_LOGE("[MQTT] 消息发布失败: rc={}, msg={}", code, msg);
}

/**
 * 订阅失败回调
 */
void MqttClient::onSubscribeFailure(void* context, MQTTAsync_failureData* response) {
    (void)context;
    int code = response ? response->code : -1;
    const char* msg = (response && response->message) ? response->message : "";
    NC_LOGE("[MQTT] 订阅失败: rc={}, msg={}", code, msg);
}

} // namespace mqtt
} // namespace nc
