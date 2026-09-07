/**
 * @file tb_topics.h
 * @brief ThingsBoard MQTT 主题定义
 *
 * 把 TB 的主题集中放在这里，方便查找和修改。
 * 加新平台时，新平台有自己的主题定义文件，互不干扰。
 */
#pragma once

#include <string>
#include <vector>

namespace iot_agent {
namespace tb {

/* ---- 订阅主题 ---- */
static constexpr const char* TOPIC_ATTRIBUTES         = "v1/devices/me/attributes";
static constexpr const char* TOPIC_RPC_REQUEST        = "v1/devices/me/rpc/request/+";
static constexpr const char* TOPIC_ATTR_RESPONSE      = "v1/devices/me/attributes/response/+";

/* ---- 发布主题模板 ---- */
static constexpr const char* TOPIC_RPC_RESPONSE_FMT   = "v1/devices/me/rpc/response/";
static constexpr const char* TOPIC_ATTR_REQUEST_FMT   = "v1/devices/me/attributes/request/";
static constexpr const char* TOPIC_TELEMETRY          = "v1/devices/me/telemetry";

/* ---- provision 主题 ---- */
static constexpr const char* TOPIC_PROVISION_REQUEST  = "/provision/request";
static constexpr const char* TOPIC_PROVISION_RESPONSE = "/provision/response";

/** 返回 TB 需要订阅的主题列表 */
inline std::vector<std::string> subscribeTopics() {
    return {
        TOPIC_ATTRIBUTES,
        TOPIC_RPC_REQUEST,
        TOPIC_ATTR_RESPONSE
    };
}

} // namespace tb
} // namespace iot_agent
