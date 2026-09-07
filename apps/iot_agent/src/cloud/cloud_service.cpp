/**
 * @file cloud_service.cpp
 * @brief 云服务实现 —— 管理与 IoT 平台的 MQTT 连接
 *
 * 和旧版的核心区别：
 *   - MqttClient 用 unique_ptr，不再裸 new/delete
 *   - 平台接口从 IIotPlatform 换成 CloudAdapter
 *   - 消息解析从回调模式改成 DeviceData 返回值模式
 *   - m_cfg 存值拷贝，不持外部指针
 *   - m_attrRequested 用 atomic<bool>
 */
#include "cloud_service.h"
#include "config/agent_config.h"
#include "cloud/platform_factory.h"
#include "router/rpc_handler.h"
#include "nc/mqtt/mqtt_client.h"
#include "nc/common/log_utils.h"
#include "cJSON.h"

namespace iot_agent {

CloudService::CloudService() = default;

CloudService::~CloudService() {
    stop();
}

/**
 * 初始化云服务
 *
 * 三步：
 *   1) 工厂创建 CloudAdapter（编译期裁剪，只编了一个平台的代码）
 *   2) adapter->authenticate() 拿凭据（TB 先查本地，没有就走 provision）
 *   3) adapter->configureMqtt() 拼好 MQTT 参数，存到 m_mqttCfg 供 start() 用
 *
 * 拿不到凭据就返回 false，main.cpp 会直接退出。
 */
bool CloudService::initialize(const AgentConfig& cfg, const std::string& deviceId) {
    /* 存值拷贝，不持外部指针 */
    m_cfgJson = cfg.rawJson();
    m_platformName = cfg.platformName();
    m_deviceId = deviceId;

    /* 1) 创建适配器（编译期只编了一个平台） */
    m_adapter = PlatformFactory::create(cfg.platformName());
    if (!m_adapter) {
        NC_LOGE("[CloudService] unsupported platform: {}", cfg.platformName().c_str());
        return false;
    }
    NC_LOGI("[CloudService] platform = {}", m_adapter->name().c_str());

    /* 2) 拿凭据 */
    if (!m_adapter->authenticate(cfg.rawJson(), deviceId, m_credential)) {
        NC_LOGE("[CloudService] authenticate failed");
        return false;
    }
    NC_LOGI("[CloudService] credential ready (len={})", m_credential.size());

    /* 3) 拼 MQTT 连接参数 + 把平台要订阅的主题加进去 */
    m_adapter->configureMqtt(m_mqttCfg, m_credential, cfg.rawJson(), deviceId);
    auto topics = m_adapter->subscribeTopics();
    for (const auto& t : topics) {
        m_mqttCfg.topics.push_back(t);
    }
    NC_LOGI("[CloudService] will subscribe {} topics", topics.size());

    return true;
}

/**
 * 设置业务层回调
 *
 * main.cpp 调这个，把 ConfigRouter / RpcRouter 的处理逻辑传进来。
 */
void CloudService::onAttributes(AttributesCallback cb) {
    m_onAttributes = std::move(cb);
}

void CloudService::onCommand(CommandCallback cb) {
    m_onCommand = std::move(cb);
}

/**
 * 启动 MQTT 连接
 *
 * 用 initialize() 时拼好的 m_mqttCfg 创建 MqttClient（unique_ptr），
 * 设回调 → 连接服务器 → 自动订阅主题。
 */
bool CloudService::start() {
    if (!m_adapter) {
        NC_LOGE("[CloudService] not initialized");
        return false;
    }

    /* 创建 MQTT 客户端（unique_ptr 管理生命周期） */
    m_mqttClient = std::unique_ptr<nc::mqtt::MqttClient>(
        new nc::mqtt::MqttClient(m_mqttCfg));

    /* 消息回调：收到消息 → onMqttMessage() → adapter 解析 → DeviceData → 业务回调 */
    m_mqttClient->setMessageCallback(
        [this](const std::string& topic, const std::string& payload) {
            onMqttMessage(topic, payload);
        });

    /* 状态回调：连接状态变化 → onMqttState() */
    m_mqttClient->setStateCallback(
        [this](nc::mqtt::MqttState state, const std::string& info) {
            onMqttState(state, info);
        });

    /* 启动（连接服务器、自动订阅主题） */
    if (!m_mqttClient->start()) {
        NC_LOGE("[CloudService] MQTT start failed");
        m_mqttClient.reset();  /* unique_ptr 自动释放 */
        return false;
    }

    NC_LOGI("[CloudService] started, address={}", m_mqttCfg.address.c_str());
    return true;
}

/**
 * 停止 MQTT 连接
 *
 * unique_ptr.reset() 自动调 MqttClient 析构，不用手动 delete。
 * 重置"已拉取属性"标记，下次 start() 后会重新拉一次。
 */
void CloudService::stop() {
    if (m_mqttClient) {
        m_mqttClient->stop();
        m_mqttClient.reset();
    }
    m_attrRequested.store(false);
}

/**
 * 发送 MQTT 消息
 *
 * 供外部（如 RpcRouter 发 RPC 回复）调用，
 * 底层走 adapter->publish()，直接传 MqttClient& 进去。
 */
bool CloudService::publish(const std::string& topic, const std::string& payload) {
    if (!m_adapter || !m_mqttClient) return false;
    return m_adapter->publish(*m_mqttClient, topic, payload) > 0;
}

/** 遥测上报：主题问平台适配器要，上层不用认识 tb_topics.h */
bool CloudService::publishTelemetry(const std::string& payload) {
    if (!m_adapter) return false;
    return publish(m_adapter->telemetryTopic(), payload);
}

/** 属性上报：同上 */
bool CloudService::publishAttributes(const std::string& payload) {
    if (!m_adapter) return false;
    return publish(m_adapter->attributesTopic(), payload);
}

/** 查询 MQTT 是否已连接 */
bool CloudService::isConnected() const {
    if (!m_mqttClient) return false;
    return m_mqttClient->getState() == nc::mqtt::MqttState::Connected;
}

/** 获取平台名 */
std::string CloudService::platformName() const {
    return m_adapter ? m_adapter->name() : "";
}

/**
 * MQTT 消息回调
 *
 * 收到消息后交给 adapter 解析成 DeviceData 列表，
 * 然后逐条调 dispatchData() 分发到业务层回调。
 *
 * 和旧版的区别：不再传回调给平台，而是平台返回 DeviceData，这里再分发。
 * 好处是平台代码不依赖业务层的回调类型定义。
 */
void CloudService::onMqttMessage(const std::string& topic, const std::string& payload) {
    if (!m_adapter) return;

    NC_LOGI("[CloudService] recv: topic={} len={} payload={}",
            topic.c_str(), payload.size(), payload.c_str());

    auto dataList = m_adapter->parseMessage(topic, payload);
    for (const auto& data : dataList) {
        dispatchData(data);
    }
}

/**
 * 根据 DeviceData 类型分发到对应业务回调
 *
 * Attributes / AttributesResponse → m_onAttributes（ConfigRouter 存本地 + AttributeHandler 分发）
 * RpcRequest → m_onCommand（RpcHandler 处理）
 */
void CloudService::dispatchData(const DeviceData& data) {
    const char* typeName = "?";
    switch (data.type) {
        case DeviceData::Type::Attributes:         typeName = "Attributes"; break;
        case DeviceData::Type::AttributesResponse:  typeName = "AttrResponse"; break;
        case DeviceData::Type::RpcRequest:          typeName = "RpcRequest"; break;
    }
    NC_LOGI("[CloudService] dispatch: type={} source={}", typeName, data.source.c_str());

    switch (data.type) {
        case DeviceData::Type::Attributes:
        case DeviceData::Type::AttributesResponse:
            if (m_onAttributes) {
                m_onAttributes(data.rawJson);
            }
            break;

        case DeviceData::Type::RpcRequest: {
            if (m_onCommand) {
                /* 把 DeviceData 转成 RpcRequest（RpcHandler 认这个结构体） */
                RpcRequest req;
                req.method = data.method;
                req.paramsJson = data.paramsJson;
                req.requestId = data.requestId;
                req.source = data.source;
                m_onCommand(req);
            }
            break;
        }
    }
}

/**
 * MQTT 状态回调
 *
 * 首次连接成功后，主动拉取服务器属性（requestAttributes）。
 * 只拉一次，重连不重复拉——避免 OTA 字段重复触发、避免和配置推送产生竞态。
 *
 * m_attrRequested 用 atomic<bool>，因为这个回调在 MQTT 内部线程执行，
 * 和 stop() 里的主线程可能并发读写。
 */
void CloudService::onMqttState(nc::mqtt::MqttState state, const std::string& info) {
    /* 把状态枚举转成字符串，方便日志里看 */
    const char* s = "?";
    switch (state) {
        case nc::mqtt::MqttState::Disconnected: s = "disconnected"; break;
        case nc::mqtt::MqttState::Connecting:   s = "connecting"; break;
        case nc::mqtt::MqttState::Connected:    s = "connected"; break;
        case nc::mqtt::MqttState::Reconnecting: s = "reconnecting"; break;
    }
    NC_LOGI("[CloudService] MQTT state={} info={}", s, info.c_str());

    /*
     * 首次连接成功 → 拉取服务器属性
     *
     * 为什么只拉一次？
     *   - 设备启动时不知道服务器有什么配置，需要主动拉一次
     *   - 重连后服务器会主动推送最新配置，不需要重复拉
     *   - 避免 OTA 字段重复触发、避免和配置推送产生竞态
     */
    if (state == nc::mqtt::MqttState::Connected && !m_attrRequested.load()) {
        m_attrRequested.store(true);
        NC_LOGI("[CloudService] first connect, requesting server attributes");
        if (m_adapter && m_mqttClient) {
            m_adapter->requestAttributes(*m_mqttClient);
        }
    }
}

} // namespace iot_agent
