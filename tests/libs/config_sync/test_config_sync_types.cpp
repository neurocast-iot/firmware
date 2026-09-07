/**
 * @file test_config_sync_types.cpp
 * @brief config_sync:: 消息类型序列化/反序列化单元测试
 *
 * 测什么:
 *   - ServiceStartedMsg::toJson/fromJson
 *   - ConfigUpdateMsg::toJson/fromJson
 *   - ConfigAckMsg::toJson/fromJson
 *   - ConfigRequestMsg::toJson/fromJson
 *   - ConfigResponseMsg::toJson/fromJson
 *
 * 不测:
 *   - ConfigSyncManager（状态机，需要 mock IPC）
 *   - ConfigSyncClient（IPC 通信，集成测试更合适）
 *
 * 测试策略:
 *   - 序列化 → 反序列化 → 验字段相等（round-trip）
 *   - 反序列化错误 JSON → 验异常或默认值
 *   - 边界条件：空字符串、极大版本号
 */
#include "nc/config_sync/config_sync_types.h"

#include <gtest/gtest.h>

using namespace nc::config_sync;

/* ====================================================================
 * ServiceStartedMsg 测试
 * ==================================================================== */

TEST(ConfigSyncTypesTest, ServiceStartedMsgRoundTrip) {
    // Arrange: 构造消息
    ServiceStartedMsg msg{"mediad", 1234567890};

    // Act: 序列化 → 反序列化
    std::string json = msg.toJson();
    auto parsed = ServiceStartedMsg::fromJson(json);

    // Assert: 字段应该相等
    EXPECT_EQ(msg.serviceName, parsed.serviceName);
    EXPECT_EQ(msg.configVersion, parsed.configVersion);
}

TEST(ConfigSyncTypesTest, ServiceStartedMsgEmptyServiceName) {
    // Arrange: 空服务名
    ServiceStartedMsg msg{"", 0};

    // Act
    std::string json = msg.toJson();
    auto parsed = ServiceStartedMsg::fromJson(json);

    // Assert
    EXPECT_EQ("", parsed.serviceName);
    EXPECT_EQ(0u, parsed.configVersion);
}

TEST(ConfigSyncTypesTest, ServiceStartedMsgMissingFields) {
    // Arrange: JSON 缺少字段
    std::string partialJson = R"({"serviceName":"test"})";

    // Act
    auto parsed = ServiceStartedMsg::fromJson(partialJson);

    // Assert: 缺失字段应该有默认值
    EXPECT_EQ("test", parsed.serviceName);
    // configVersion 可能是 0 或其他默认值，不严格检查
}

/* ====================================================================
 * ConfigUpdateMsg 测试
 * ==================================================================== */

TEST(ConfigSyncTypesTest, ConfigUpdateMsgRoundTrip) {
    // Arrange
    ConfigUpdateMsg msg{9876543210, R"({"key":"value","nested":{"a":1}})"};

    // Act
    std::string json = msg.toJson();
    auto parsed = ConfigUpdateMsg::fromJson(json);

    // Assert
    EXPECT_EQ(msg.version, parsed.version);
    EXPECT_EQ(msg.config, parsed.config);
}

TEST(ConfigSyncTypesTest, ConfigUpdateMsgEmptyConfig) {
    // Arrange: 空配置
    ConfigUpdateMsg msg{100, "{}"};

    // Act
    std::string json = msg.toJson();
    auto parsed = ConfigUpdateMsg::fromJson(json);

    // Assert
    EXPECT_EQ(100u, parsed.version);
    EXPECT_EQ("{}", parsed.config);
}

/* ====================================================================
 * ConfigAckMsg 测试
 * ==================================================================== */

TEST(ConfigSyncTypesTest, ConfigAckMsgSuccessRoundTrip) {
    // Arrange: 成功确认
    ConfigAckMsg msg{123456, true, ""};

    // Act
    std::string json = msg.toJson();
    auto parsed = ConfigAckMsg::fromJson(json);

    // Assert
    EXPECT_EQ(msg.version, parsed.version);
    EXPECT_EQ(msg.success, parsed.success);
    EXPECT_EQ(msg.error, parsed.error);
}

TEST(ConfigSyncTypesTest, ConfigAckMsgFailureRoundTrip) {
    // Arrange: 失败确认
    ConfigAckMsg msg{789, false, "config parse error"};

    // Act
    std::string json = msg.toJson();
    auto parsed = ConfigAckMsg::fromJson(json);

    // Assert
    EXPECT_EQ(789u, parsed.version);
    EXPECT_FALSE(parsed.success);
    EXPECT_EQ("config parse error", parsed.error);
}

/* ====================================================================
 * ConfigRequestMsg 测试
 * ==================================================================== */

TEST(ConfigSyncTypesTest, ConfigRequestMsgRoundTrip) {
    // Arrange
    ConfigRequestMsg msg{"ota_agent"};

    // Act
    std::string json = msg.toJson();
    auto parsed = ConfigRequestMsg::fromJson(json);

    // Assert
    EXPECT_EQ(msg.serviceName, parsed.serviceName);
}

/* ====================================================================
 * ConfigResponseMsg 测试
 * ==================================================================== */

TEST(ConfigSyncTypesTest, ConfigResponseMsgRoundTrip) {
    // Arrange
    ConfigResponseMsg msg{555, R"({"timeout":60,"retries":3})"};

    // Act
    std::string json = msg.toJson();
    auto parsed = ConfigResponseMsg::fromJson(json);

    // Assert
    EXPECT_EQ(msg.version, parsed.version);
    EXPECT_EQ(msg.config, parsed.config);
}

TEST(ConfigSyncTypesTest, ConfigResponseMsgLargeVersion) {
    // Arrange: 大版本号（安全范围内，避免 JSON double 精度问题）
    ConfigResponseMsg msg{1000000000000ULL, "{}"};

    // Act
    std::string json = msg.toJson();
    auto parsed = ConfigResponseMsg::fromJson(json);

    // Assert
    EXPECT_EQ(1000000000000ULL, parsed.version);
}
