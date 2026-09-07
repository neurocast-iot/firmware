/**
 * @file device_data.h
 * @brief 统一数据模型 —— 业务层只处理这个结构体，不管云端是哪个平台
 *
 * 为什么需要这个？
 *   TB 推送的 JSON 格式和 AWS Shadow 格式完全不一样，
 *   但如果业务层（ConfigRouter / RpcRouter）只认 DeviceData，
 *   那换平台时业务层一行不用改。
 *
 * 数据流：
 *   云端消息 → CloudAdapter.parseMessage() → DeviceData → 业务层
 *   业务层 → DeviceData → CloudAdapter → 云端消息
 */
#pragma once

#include <string>

namespace iot_agent {

/**
 * 统一数据模型
 *
 * 每条从云端收到的消息，都会被平台适配器解析成一个或多个 DeviceData。
 * 业务层（ConfigRouter / RpcHandler）只和 DeviceData 打交道。
 */
struct DeviceData {
    /** 消息类型 */
    enum class Type {
        Attributes,         /* 属性推送（配置变更） */
        AttributesResponse, /* 属性响应（requestAttributes 的回复） */
        RpcRequest          /* RPC 指令 */
    };

    Type        type = Type::Attributes;

    /* ---- 通用字段 ---- */
    std::string rawJson;        /* 原始 JSON（给 ConfigRouter 存本地用） */
    std::string source;         /* 来源平台名（"thingsboard" / "aws" / ...） */

    /* ---- RPC 专用字段 ---- */
    std::string method;         /* RPC 方法名（如 "setCameraConfig"） */
    std::string paramsJson;     /* RPC 参数 JSON 字符串 */
    std::string requestId;      /* 请求 ID（回复时用） */
};

} // namespace iot_agent
