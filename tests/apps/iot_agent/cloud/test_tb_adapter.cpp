/**
 * @file test_tb_adapter.cpp
 * @brief TBAdapter 纯逻辑测试：parseMessage 消息解析 + configureMqtt 配置映射
 *
 * 不需要真实 MQTT 连接，只测 JSON 解析和参数映射逻辑。
 * 通过桩头文件提供 MqttConfig / MqttClient 类型声明，绕过 Paho 依赖。
 */
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "cloud/thingsboard/tb_adapter.h"
#include "cloud/thingsboard/tb_topics.h"
#include "nc/mqtt/mqtt_client.h"

using namespace iot_agent;

/* ====================================================================
 * parseMessage 测试
 * ==================================================================== */

/** 属性推送：没有 shared 包裹 → rawJson 保留整个 payload */
TEST(TBAdapterParseMessage, AttributePushNoShared) {
    TBAdapter adapter;
    std::string payload = R"({"main_codec":"h264","sub_codec":"h265"})";
    auto result = adapter.parseMessage("v1/devices/me/attributes", payload);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].type, DeviceData::Type::Attributes);
    EXPECT_EQ(result[0].rawJson, payload);
    EXPECT_EQ(result[0].source, "thingsboard");
}

/** 属性推送：有 shared 包裹 → rawJson 只保留 shared 里的内容 */
TEST(TBAdapterParseMessage, AttributePushWithShared) {
    TBAdapter adapter;
    std::string payload = R"({"shared":{"main_codec":"h264","sub_codec":"h265"}})";
    auto result = adapter.parseMessage("v1/devices/me/attributes", payload);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].type, DeviceData::Type::Attributes);
    EXPECT_EQ(result[0].source, "thingsboard");
    /* shared 里的内容被提取出来，不包含外层 "shared" 节点 */
    EXPECT_NE(result[0].rawJson.find("main_codec"), std::string::npos);
    EXPECT_EQ(result[0].rawJson.find("shared"), std::string::npos);
}

/** 属性响应：主题含 attributes/response/ → 类型为 AttributesResponse */
TEST(TBAdapterParseMessage, AttributeResponse) {
    TBAdapter adapter;
    std::string payload = R"({"main_codec":"h264"})";
    auto result = adapter.parseMessage("v1/devices/me/attributes/response/12345", payload);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].type, DeviceData::Type::AttributesResponse);
    EXPECT_EQ(result[0].source, "thingsboard");
}

/** RPC 指令：从主题提取 requestId，从 JSON 提取 method 和 params */
TEST(TBAdapterParseMessage, RpcRequestFull) {
    TBAdapter adapter;
    std::string payload = R"({"method":"restart","params":{"delay":1}})";
    auto result = adapter.parseMessage("v1/devices/me/rpc/request/42", payload);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].type, DeviceData::Type::RpcRequest);
    EXPECT_EQ(result[0].requestId, "42");
    EXPECT_EQ(result[0].method, "restart");
    EXPECT_EQ(result[0].source, "thingsboard");
    /* params 被序列化成 JSON 字符串 */
    EXPECT_NE(result[0].paramsJson.find("delay"), std::string::npos);
}

/** RPC 指令：没有 params 字段 → paramsJson 为空 */
TEST(TBAdapterParseMessage, RpcRequestNoParams) {
    TBAdapter adapter;
    std::string payload = R"({"method":"stopLiveStream"})";
    auto result = adapter.parseMessage("v1/devices/me/rpc/request/99", payload);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].type, DeviceData::Type::RpcRequest);
    EXPECT_EQ(result[0].requestId, "99");
    EXPECT_EQ(result[0].method, "stopLiveStream");
    EXPECT_TRUE(result[0].paramsJson.empty());
}

/** RPC 指令：params 是基本类型（字符串）→ 也能序列化 */
TEST(TBAdapterParseMessage, RpcRequestStringParams) {
    TBAdapter adapter;
    std::string payload = R"({"method":"uploadFile","params":"simple_value"})";
    auto result = adapter.parseMessage("v1/devices/me/rpc/request/7", payload);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].method, "uploadFile");
    /* params 是字符串 "simple_value"，序列化后应该包含它 */
    EXPECT_NE(result[0].paramsJson.find("simple_value"), std::string::npos);
}

/** 非法 JSON → 返回空列表 */
TEST(TBAdapterParseMessage, InvalidJsonReturnsEmpty) {
    TBAdapter adapter;
    auto result = adapter.parseMessage("v1/devices/me/attributes", "not json at all");
    EXPECT_TRUE(result.empty());
}

/** 未知主题 → 返回空列表 */
TEST(TBAdapterParseMessage, UnknownTopicReturnsEmpty) {
    TBAdapter adapter;
    auto result = adapter.parseMessage("some/random/topic", R"({"key":"value"})");
    EXPECT_TRUE(result.empty());
}

/** 空 payload → 返回空列表 */
TEST(TBAdapterParseMessage, EmptyPayloadReturnsEmpty) {
    TBAdapter adapter;
    auto result = adapter.parseMessage("v1/devices/me/attributes", "");
    EXPECT_TRUE(result.empty());
}

/* ====================================================================
 * configureMqtt 测试
 * ==================================================================== */

/** ssl:// URL → enableSsl = true */
TEST(TBAdapterConfigureMqtt, SslUrlEnablesSsl) {
    TBAdapter adapter;
    nc::mqtt::MqttConfig cfg;
    std::string cfgJson = R"({"mqtt":{"url":"ssl://example.com:8883"}})";
    adapter.configureMqtt(cfg, "mytoken", cfgJson, "device001");

    EXPECT_EQ(cfg.address, "ssl://example.com:8883");
    EXPECT_TRUE(cfg.enableSsl);
}

/** tls:// URL → enableSsl = true */
TEST(TBAdapterConfigureMqtt, TlsUrlEnablesSsl) {
    TBAdapter adapter;
    nc::mqtt::MqttConfig cfg;
    std::string cfgJson = R"({"mqtt":{"url":"tls://example.com:8883"}})";
    adapter.configureMqtt(cfg, "mytoken", cfgJson, "device001");

    EXPECT_EQ(cfg.address, "tls://example.com:8883");
    EXPECT_TRUE(cfg.enableSsl);
}

/** tcp:// URL → enableSsl = false */
TEST(TBAdapterConfigureMqtt, TcpUrlDisablesSsl) {
    TBAdapter adapter;
    nc::mqtt::MqttConfig cfg;
    std::string cfgJson = R"({"mqtt":{"url":"tcp://example.com:1883"}})";
    adapter.configureMqtt(cfg, "mytoken", cfgJson, "device001");

    EXPECT_EQ(cfg.address, "tcp://example.com:1883");
    EXPECT_FALSE(cfg.enableSsl);
}

/** credential → username，password 空 */
TEST(TBAdapterConfigureMqtt, CredentialBecomesUsername) {
    TBAdapter adapter;
    nc::mqtt::MqttConfig cfg;
    std::string cfgJson = R"({"mqtt":{"url":"tcp://localhost:1883"}})";
    adapter.configureMqtt(cfg, "secret_token", cfgJson, "device001");

    EXPECT_EQ(cfg.username, "secret_token");
    EXPECT_TRUE(cfg.password.empty());
}

/** clientId = deviceName + "_live"，device_name 覆盖默认值 */
TEST(TBAdapterConfigureMqtt, DeviceNameInClientId) {
    TBAdapter adapter;
    nc::mqtt::MqttConfig cfg;
    std::string cfgJson = R"({"mqtt":{"url":"tcp://localhost:1883"},"device_name":"mycam"})";
    adapter.configureMqtt(cfg, "tok", cfgJson, "device001");

    EXPECT_EQ(cfg.clientId, "mycam_live");
}

/** 没有 device_name → 用 deviceId 作为 clientId 前缀 */
TEST(TBAdapterConfigureMqtt, DeviceIdFallbackForClientId) {
    TBAdapter adapter;
    nc::mqtt::MqttConfig cfg;
    std::string cfgJson = R"({"mqtt":{"url":"tcp://localhost:1883"}})";
    adapter.configureMqtt(cfg, "tok", cfgJson, "fallback_id");

    EXPECT_EQ(cfg.clientId, "fallback_id_live");
}

/** 没有 mqtt.url → 用默认地址 */
TEST(TBAdapterConfigureMqtt, DefaultUrlFallback) {
    TBAdapter adapter;
    nc::mqtt::MqttConfig cfg;
    std::string cfgJson = R"({"other_field":"value"})";
    adapter.configureMqtt(cfg, "tok", cfgJson, "device001");

    /* 默认地址在 tb_adapter.cpp 里硬编码为 "ssl://mqtt.example.com:8883" */
    EXPECT_EQ(cfg.address, "ssl://mqtt.example.com:8883");
    EXPECT_TRUE(cfg.enableSsl);  /* 默认地址是 ssl://，所以 SSL 开启 */
}

/** 固定参数验证：keepAlive / autoReconnect / timeout 等 */
TEST(TBAdapterConfigureMqtt, FixedParameters) {
    TBAdapter adapter;
    nc::mqtt::MqttConfig cfg;
    std::string cfgJson = R"({"mqtt":{"url":"tcp://localhost:1883"}})";
    adapter.configureMqtt(cfg, "tok", cfgJson, "dev1");

    EXPECT_EQ(cfg.keepAliveInterval, 60);
    EXPECT_TRUE(cfg.autoReconnect);
    EXPECT_EQ(cfg.initialReconnectIntervalMs, 2000);
    EXPECT_EQ(cfg.maxReconnectIntervalMs, 30000);
    EXPECT_EQ(cfg.connectTimeoutMs, 10000);
    EXPECT_EQ(cfg.defaultQos, 1);
}

/* ====================================================================
 * subscribeTopics / telemetryTopic / attributesTopic 测试
 * ==================================================================== */

TEST(TBAdapterTopics, SubscribeTopicsMatchDefinitions) {
    TBAdapter adapter;
    auto topics = adapter.subscribeTopics();

    ASSERT_EQ(topics.size(), 3u);
    EXPECT_EQ(topics[0], tb::TOPIC_ATTRIBUTES);
    EXPECT_EQ(topics[1], tb::TOPIC_RPC_REQUEST);
    EXPECT_EQ(topics[2], tb::TOPIC_ATTR_RESPONSE);
}

TEST(TBAdapterTopics, TelemetryAndAttributesTopic) {
    TBAdapter adapter;
    EXPECT_EQ(adapter.telemetryTopic(), tb::TOPIC_TELEMETRY);
    EXPECT_EQ(adapter.attributesTopic(), tb::TOPIC_ATTRIBUTES);
}

TEST(TBAdapterTopics, NameIsThingsboard) {
    TBAdapter adapter;
    EXPECT_EQ(adapter.name(), "thingsboard");
}
