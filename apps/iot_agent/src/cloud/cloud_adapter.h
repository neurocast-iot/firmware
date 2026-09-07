/**
 * @file cloud_adapter.h
 * @brief 云平台适配器统一接口
 *
 * 每个云平台（TB / AWS / EMQX / Azure）实现这个接口。
 * CloudService 只通过这个接口和平台交互，不关心具体平台是谁。
 *
 * 和旧 IIotPlatform 的区别：
 *   - 去掉了 setMqttClient()（不再反向依赖）
 *   - 需要 MQTT 客户端的方法，直接把 MqttClient& 传进来
 *   - parseMessage() 返回 DeviceData（统一数据模型），不再用回调
 *   - authenticate() 替代 acquireToken()（AWS 不是"拿 token"，是"加载证书"）
 *
 * 编译期裁剪：
 *   CMakeLists.txt 里设 IOT_AGENT_PLATFORM=thingsboard，
 *   只编 TB 的代码，AWS/EMQX 的代码完全不进二进制。
 */
#pragma once

#include "device_data.h"

#include <memory>
#include <string>
#include <vector>

namespace nc { namespace mqtt { class MqttClient; struct MqttConfig; } }

namespace iot_agent {

class CloudAdapter {
public:
    virtual ~CloudAdapter() = default;

    /** 平台标识（"thingsboard" / "aws" / "emqx" / ...） */
    virtual std::string name() const = 0;

    /**
     * 认证（拿凭据）
     *
     * 不同平台拿凭据的方式不一样：
     *   - TB：用 deviceKey/Secret 走 provision 换 accessToken
     *   - AWS：加载 X.509 证书文件
     *   - EMQX：调业务后端 HTTP 接口拿 JWT
     *
     * @param cfgJson       iot_agent 配置 JSON
     * @param deviceId      设备 ID
     * @param credentialOut 输出参数：拿到的凭据（token / 证书路径 / JWT）
     * @return 是否成功
     */
    virtual bool authenticate(const std::string& cfgJson,
                              const std::string& deviceId,
                              std::string& credentialOut) = 0;

    /**
     * 用凭据配 MQTT 连接参数
     *
     * @param mqttCfg    输出参数：MQTT 连接配置
     * @param credential 认证拿到的凭据
     * @param cfgJson    iot_agent 配置 JSON
     * @param deviceId   设备 ID（拼进 clientId）
     */
    virtual void configureMqtt(nc::mqtt::MqttConfig& mqttCfg,
                               const std::string& credential,
                               const std::string& cfgJson,
                               const std::string& deviceId) = 0;

    /** 返回该平台需要订阅的主题列表 */
    virtual std::vector<std::string> subscribeTopics() const = 0;

    /**
     * 遥测上报主题（各平台不一样：TB 是 v1/devices/me/telemetry）
     *
     * 上层（IpcEventHandler / main）要发遥测时，不直接写死主题名，
     * 而是通过 CloudService::publishTelemetry() 间接用这个值——
     * 主题名是平台细节，只许出现在平台自己的目录里。
     */
    virtual std::string telemetryTopic() const = 0;

    /** 属性上报主题（TB 是 v1/devices/me/attributes），语义同上 */
    virtual std::string attributesTopic() const = 0;

    /**
     * 解析收到的 MQTT 消息 → 统一 DeviceData
     *
     * 不同平台的消息格式不一样（TB 用 v1/devices/me/...，AWS 用 $aws/things/...），
     * 但解析完后都变成 DeviceData，业务层不用关心平台差异。
     *
     * @param topic   MQTT 主题
     * @param payload 消息内容（JSON 字符串）
     * @return 解析出的 DeviceData 列表（一条消息可能解析出多条 DeviceData）
     */
    virtual std::vector<DeviceData> parseMessage(const std::string& topic,
                                                  const std::string& payload) = 0;

    /**
     * 主动请求服务器下发属性（首次连接后调一次）
     *
     * @param client MQTT 客户端引用（由 CloudService 管理生命周期）
     * @return 请求是否发送成功
     */
    virtual bool requestAttributes(nc::mqtt::MqttClient& client) = 0;

    /**
     * 发送 MQTT 消息
     *
     * @param client  MQTT 客户端引用
     * @param topic   MQTT 主题
     * @param payload JSON 字符串
     * @param qos     QoS 等级
     * @return 1=已发送, 2=已缓存待重发, 0=失败
     */
    virtual int publish(nc::mqtt::MqttClient& client,
                        const std::string& topic,
                        const std::string& payload,
                        int qos = 1) = 0;
};

} // namespace iot_agent
