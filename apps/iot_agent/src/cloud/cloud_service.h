/**
 * @file cloud_service.h
 * @brief 云服务 —— 管理与 IoT 平台的连接（MQTT / 认证 / 消息收发）
 *
 * 职责：
 *   1) 通过 PlatformFactory 创建 CloudAdapter 实例
 *   2) 调 adapter->authenticate() 拿凭据（TB 走 provision；EMQX 走 JWT；...）
 *   3) 管理 MQTT 客户端生命周期（unique_ptr，连接 / 断开 / 重连）
 *   4) 消息分发：收到消息 → adapter->parseMessage() → DeviceData → 业务层回调
 *
 * 和旧版的区别：
 *   - MqttClient 用 unique_ptr 管理，不再裸 new/delete
 *   - m_attrRequested 用 atomic<bool>，MQTT 回调线程读写安全
 *   - m_cfg 存值拷贝而非指针，避免悬空引用
 *   - 平台接口从 IIotPlatform 换成 CloudAdapter（不再 setMqttClient 反向依赖）
 */
#pragma once

#include "device_data.h"
#include "nc/mqtt/mqtt_client.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace iot_agent {

class AgentConfig;
class CloudAdapter;
struct RpcRequest;

class CloudService {
public:
    /* 业务层回调 */
    using AttributesCallback = std::function<void(const std::string& rawJson)>;
    using CommandCallback    = std::function<void(const RpcRequest&)>;

    CloudService();
    ~CloudService();

    /**
     * 初始化云服务
     *
     * 流程：
     *   1) 根据配置里的 platform 字段创建 CloudAdapter 实例
     *   2) 调 adapter->authenticate() 拿凭据
     *   3) 调 adapter->configureMqtt() 拼 MQTT 连接参数（保存供 start() 用）
     *
     * @param cfg      iot_agent 配置（存值拷贝，不持指针）
     * @param deviceId 设备 ID（来自 DeviceService）
     * @return 是否成功
     */
    bool initialize(const AgentConfig& cfg, const std::string& deviceId);

    /**
     * 注册业务层回调（每个事件单独注册，加新事件只加一个方法，不改已有签名）
     */
    void onAttributes(AttributesCallback cb);
    void onCommand(CommandCallback cb);

    /**
     * 启动 MQTT 连接
     *
     * 创建 MqttClient（unique_ptr），设置回调，连接服务器，订阅平台主题。
     * 首次连接成功后会自动拉取服务器属性（requestAttributes）。
     *
     * @return 是否启动成功
     */
    bool start();

    /** 停止 MQTT 连接（unique_ptr 自动释放客户端内存） */
    void stop();

    /**
     * 发送 MQTT 消息
     *
     * 供外部（如 RpcRouter）调用，底层走 adapter->publish()。
     *
     * @param topic   MQTT 主题
     * @param payload JSON 字符串
     * @return 是否发送成功
     */
    bool publish(const std::string& topic, const std::string& payload);

    /**
     * 上报遥测数据（主题由平台适配器决定，调用方不用关心）
     *
     * 上层模块（IpcEventHandler 等）要发遥测统一走这里，
     * 不允许直接 include 平台主题头文件（tb_topics.h 等）。
     */
    bool publishTelemetry(const std::string& payload);

    /** 上报属性/设备状态（主题由平台适配器决定），语义同上 */
    bool publishAttributes(const std::string& payload);

    /** 查询 MQTT 是否已连接 */
    bool isConnected() const;

    /** 获取平台名（"thingsboard" / "emqx" / ...） */
    std::string platformName() const;

private:
    /** MQTT 消息回调 → 交给 adapter 解析成 DeviceData → 分发到业务回调 */
    void onMqttMessage(const std::string& topic, const std::string& payload);

    /** MQTT 状态回调 → 首次连接成功后拉取属性 */
    void onMqttState(nc::mqtt::MqttState state, const std::string& info);

    /** 根据 DeviceData 类型分发到对应业务回调 */
    void dispatchData(const DeviceData& data);

    std::unique_ptr<CloudAdapter> m_adapter;

    /* MQTT 客户端用 unique_ptr 管理，stop() 时 reset 自动释放 */
    std::unique_ptr<nc::mqtt::MqttClient> m_mqttClient;

    /* 业务层回调 */
    AttributesCallback m_onAttributes;
    CommandCallback m_onCommand;

    /* 配置值拷贝（initialize 时保存，start 时用，不持外部指针） */
    std::string m_cfgJson;
    std::string m_platformName;
    std::string m_deviceId;
    std::string m_credential;

    /* MQTT 连接参数（initialize 时让 adapter 拼好，start() 时用） */
    nc::mqtt::MqttConfig m_mqttCfg;

    /* 是否已拉取过属性（首次连接拉一次），atomic 因为 MQTT 回调在另一个线程 */
    std::atomic<bool> m_attrRequested{false};
};

} // namespace iot_agent
