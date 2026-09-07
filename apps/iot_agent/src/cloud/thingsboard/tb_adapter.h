/**
 * @file tb_adapter.h
 * @brief ThingsBoard 适配器 —— 实现 CloudAdapter 接口
 *
 * TB 对接要点：
 *   - 认证：走 provision 流程（deviceKey/Secret → accessToken），已拆到 tb_provision.cpp
 *   - MQTT 登录：username = accessToken，password 空（TB 的规矩）
 *   - 主题：v1/devices/me/... 系列，定义在 tb_topics.h
 *   - 消息格式：attributes 推送 / RPC request / attributes response
 *
 * 和旧 ThingsBoardPlatform 的区别：
 *   - 不再存 MqttClient* 指针，publish/requestAttributes 直接传 MqttClient&
 *   - handleMessage → parseMessage，返回 DeviceData 而非用回调
 *   - provision 逻辑拆到 tb_provision.cpp，这里只调函数
 */
#pragma once

#include "cloud/cloud_adapter.h"

#include <atomic>

namespace iot_agent {

class TBAdapter : public CloudAdapter {
public:
    std::string name() const override { return "thingsboard"; }

    bool authenticate(const std::string& cfgJson,
                      const std::string& deviceId,
                      std::string& credentialOut) override;

    void configureMqtt(nc::mqtt::MqttConfig& mqttCfg,
                       const std::string& credential,
                       const std::string& cfgJson,
                       const std::string& deviceId) override;

    std::vector<std::string> subscribeTopics() const override;

    std::string telemetryTopic() const override;
    std::string attributesTopic() const override;

    std::vector<DeviceData> parseMessage(const std::string& topic,
                                          const std::string& payload) override;

    bool requestAttributes(nc::mqtt::MqttClient& client) override;

    int publish(nc::mqtt::MqttClient& client,
                const std::string& topic,
                const std::string& payload,
                int qos = 1) override;

private:
    /* 属性请求序号，每次 requestAttributes 自增，TB 用来匹配 response */
    std::atomic<int> m_requestId{0};
};

} // namespace iot_agent
