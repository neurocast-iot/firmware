/**
 * result.h
 * lib-mq 错误码体系
 *
 * 定义统一的错误码枚举和返回结果结构体，用于替代 bool 返回值。
 * 兼容 GCC 5.5（不使用 C++23 的 std::expected）。
 *
 * 使用示例：
 *   MqResult result = manager.send(type, payload);
 *   if (result) {
 *       // 发送成功
 *   } else {
 *       log("发送失败: %s, code=%d", result.message.c_str(), (int)result.code);
 *   }
 */
#pragma once
#include <string>

namespace libmq {

/**
 * 日志级别枚举
 *
 * 用于日志回调函数中标识日志级别。
 * 调用方可根据级别将日志路由到不同的输出目标（如 spdlog）。
 */
enum class LogLevel {
    Debug = 0,  // 调试信息，详细执行流程
    Info = 1,   // 一般信息，正常操作记录
    Warn = 2,   // 警告信息，潜在问题但不影响运行
    Error = 3   // 错误信息，操作失败或异常
};

/**
 * 错误码枚举
 *
 * 覆盖所有可能的错误场景，便于调用方根据不同错误码做出处理。
 */
enum class MqErrorCode {
    Success = 0,           // 操作成功
    NotInitialized,        // 队列未初始化
    AlreadyRunning,        // 队列已在运行
    EndpointInUse,         // 端点已被占用
    Timeout,               // 操作超时
    SendFailed,            // 发送失败
    RecvFailed,            // 接收失败
    EFSMError,             // ZeroMQ状态机错误(REQ/REP模式违反)
    ZmqError,              // ZeroMQ 底层错误
    InvalidConfig,         // 配置无效
    CallbackError          // 回调函数执行异常
};

/**
 * 统一返回结果
 *
 * 封装操作结果、错误码和错误描述。
 * 支持隐式转换为 bool，方便 if (result) 写法。
 */
struct MqResult {
    bool ok;                    // 是否成功
    MqErrorCode code;           // 错误码
    std::string message;        // 错误描述（调试用）
    std::string value;          // 成功时的返回值（如接收到的消息）

    /**
     * 构造成功结果
     * 
     * @param value 可选的返回值
     * @return 成功的 MqResult
     */
    static MqResult success(std::string value = "") {
        return {true, MqErrorCode::Success, "", std::move(value)};
    }

    /**
     * 构造失败结果
     * 
     * @param code 错误码
     * @param message 错误描述
     * @return 失败的 MqResult
     */
    static MqResult fail(MqErrorCode code, std::string message = "") {
        return {false, code, std::move(message), ""};
    }

    /** 隐式转换为 bool，方便 if (result) 写法 */
    explicit operator bool() const { return ok; }
};

/**
 * 将错误码转为可读字符串
 * 
 * @param code 错误码
 * @return 错误码对应的字符串描述
 */
inline const char* toString(MqErrorCode code) {
    switch (code) {
        case MqErrorCode::Success:       return "Success";
        case MqErrorCode::NotInitialized: return "NotInitialized";
        case MqErrorCode::AlreadyRunning: return "AlreadyRunning";
        case MqErrorCode::EndpointInUse:  return "EndpointInUse";
        case MqErrorCode::Timeout:        return "Timeout";
        case MqErrorCode::SendFailed:     return "SendFailed";
        case MqErrorCode::RecvFailed:     return "RecvFailed";
        case MqErrorCode::EFSMError:        return "EFSMError";
        case MqErrorCode::ZmqError:       return "ZmqError";
        case MqErrorCode::InvalidConfig:  return "InvalidConfig";
        case MqErrorCode::CallbackError:  return "CallbackError";
        default:                          return "Unknown";
    }
}

} // namespace libmq

