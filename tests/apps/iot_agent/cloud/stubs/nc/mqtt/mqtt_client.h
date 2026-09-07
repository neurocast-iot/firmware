/**
 * @file mqtt_client.h
 * @brief MQTT 类型桩：提供 MqttConfig / MqttState / MqttClient 声明
 *
 * x86 测试环境不编 nc::mqtt 库（Paho 只在 ARM 平台），
 * 但 tb_adapter.cpp 需要 MqttConfig 结构体定义来编译。
 * 这里只提供类型声明，不链接实现。
 */
#ifndef NC_MQTT_CLIENT_H_STUB
#define NC_MQTT_CLIENT_H_STUB

#include <string>
#include <functional>
#include <vector>

#include "MQTTAsync.h"

namespace nc {
namespace mqtt {

enum class MqttState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting
};

struct MqttConfig {
    std::string address;
    std::string clientId;
    std::string username;
    std::string password;
    int keepAliveInterval = 60;
    bool autoReconnect = true;
    int maxReconnectAttempts = 0;
    int initialReconnectIntervalMs = 2000;
    int maxReconnectIntervalMs = 30000;
    int connectTimeoutMs = 10000;
    int defaultQos = 1;
    std::vector<std::string> topics;
    bool enableAntiThundering = true;
    int maxRandomJitterMs = 5000;
    bool enableSsl = false;
    bool verifyServerCert = true;
};

class MqttClient {
public:
    using MessageCallback = std::function<void(const std::string&, const std::string&)>;
    using StateCallback = std::function<void(MqttState, const std::string&)>;

    explicit MqttClient(MqttConfig) {}
    ~MqttClient() = default;

    int publish(const std::string&, const std::string&, int) { return 0; }
    MqttState getState() const { return MqttState::Disconnected; }
};

} // namespace mqtt
} // namespace nc

#endif
