/**
 * config.h
 * lib-exporter 配置结构定义
 * 
 * 定义导出器的配置参数,包括进程名称、IPC 端点和日志级别。
 * 
 * 使用示例:
 *   ExporterConfig config("iot_live", "ipc:///tmp/iot_live_status.ipc");
 *   config.log_level = 1;  // INFO 级别
 *   config.logCallback = [](LogLevel level, const std::string& msg) {
 *       spdlog::info("[exporter] {}", msg);
 *   };
 */

#ifndef LIBEXPORTER_CONFIG_H
#define LIBEXPORTER_CONFIG_H

#include <string>
#include <functional>

namespace exporter {

/**
 * 日志级别枚举
 * 与 lib-mq 的 LogLevel 保持一致
 */
enum class LogLevel {
    Debug = 0,  ///< 调试信息
    Info = 1,   ///< 一般信息
    Warn = 2,   ///< 警告信息
    Error = 3   ///< 错误信息
};

/**
 * 日志回调函数类型
 * 与 lib-mq 的日志回调签名一致
 */
using LogCallback = std::function<void(LogLevel, const std::string&)>;

/**
 * 导出器配置结构体
 * 
 * 用于配置监控导出器的基本参数。
 */
struct ExporterConfig {
    std::string process_name;   ///< 进程名称 (用于标识,如 "iot_live")
    std::string endpoint;       ///< ZeroMQ IPC 端点 (如 "ipc:///tmp/iot_live_status.ipc")
    int log_level = 1;          ///< 日志级别 (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR)
    LogCallback logCallback;    ///< 日志回调函数 (可选,默认输出到 cerr)
    
    /**
     * 默认构造函数
     */
    ExporterConfig() = default;
    
    /**
     * 带参数构造函数
     * 
     * @param name 进程名称
     * @param ep IPC 端点路径
     */
    ExporterConfig(const std::string& name, const std::string& ep)
        : process_name(name), endpoint(ep) {}
};

} // namespace exporter

#endif // LIBEXPORTER_CONFIG_H
