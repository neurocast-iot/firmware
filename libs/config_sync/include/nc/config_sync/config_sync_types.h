/**
 * @file config_sync_types.h
 * @brief 配置同步相关的数据结构定义
 *
 * 配置同步流程中使用的消息格式定义。
 * 所有消息都是 JSON 格式，便于扩展和调试。
 */
#pragma once

#include <cstdint>
#include <string>

namespace nc {
namespace config_sync {

/**
 * 服务启动消息（服务 → iot_agent）
 *
 * 服务启动完成后发送，告知 iot_agent：
 * 1. 自己是谁（serviceName）
 * 2. 当前持有的配置版本（configVersion，0 表示无配置）
 *
 * iot_agent 收到后：
 * - 标记服务在线
 * - 如果 configVersion < 当前最新版本，主动下发最新配置
 */
struct ServiceStartedMsg {
    std::string serviceName;    // 服务名字，如 "mediad"、"ota_agent"
    uint64_t configVersion;     // 当前配置版本（last_updated_time）

    std::string toJson() const;
    static ServiceStartedMsg fromJson(const std::string& json);
};

/**
 * 配置更新消息（iot_agent → 服务）
 *
 * iot_agent 下发新配置时发送，包含：
 * 1. 配置版本号（version）
 * 2. 配置内容（config，JSON 对象）
 */
struct ConfigUpdateMsg {
    uint64_t version;           // 配置版本（last_updated_time）
    std::string config;         // 配置内容，JSON 字符串

    std::string toJson() const;
    static ConfigUpdateMsg fromJson(const std::string& json);
};

/**
 * 配置确认消息（服务 → iot_agent）
 *
 * 服务收到配置后回复，告知：
 * 1. 确认哪个版本（version）
 * 2. 是否成功（success）
 * 3. 失败原因（error，可选）
 */
struct ConfigAckMsg {
    uint64_t version;           // 确认的配置版本
    bool success;               // 是否成功应用
    std::string error;          // 失败原因（成功时为空）

    std::string toJson() const;
    static ConfigAckMsg fromJson(const std::string& json);
};

/**
 * 配置请求消息（服务 → iot_agent）
 *
 * 服务启动时如果发现自己没有配置（configVersion=0），
 * 可以主动请求 iot_agent 下发当前最新配置。
 */
struct ConfigRequestMsg {
    std::string serviceName;    // 请求配置的服务名字

    std::string toJson() const;
    static ConfigRequestMsg fromJson(const std::string& json);
};

/**
 * 配置响应消息（iot_agent → 服务）
 *
 * iot_agent 收到配置请求后回复当前最新配置。
 */
struct ConfigResponseMsg {
    uint64_t version;           // 配置版本
    std::string config;         // 配置内容，JSON 字符串

    std::string toJson() const;
    static ConfigResponseMsg fromJson(const std::string& json);
};

} // namespace config_sync
} // namespace nc
