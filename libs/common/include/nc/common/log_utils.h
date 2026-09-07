// nc::common 日志工具（门面）
// 实现后端为 spdlog（异步 + 每日轮转保留 7 天）
// 调用方用 NC_LOGx 宏，直接转发给 spdlog 原生 API（{} 格式化，无缓冲区限制）
#pragma once

#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace nc {
namespace common {

enum class LogLevel {
    kDebug,
    kInfo,
    kWarn,
    kError,
};

// 初始化日志：log_path 为空只出控制台；非空则控制台 + 文件双输出
// （文件每日 00:00 轮转、保留 7 天，父目录不存在时自动创建，
// 创建失败降级纯控制台，进程不因日志不可用而退出）
void LogInit(const std::string& log_path, LogLevel min_level);

// 获取全局 spdlog 日志器（NC_LOGx 宏内部用，业务代码不要直接调）
std::shared_ptr<spdlog::logger> getLogger();

// NC_LOGx 宏：带源码位置（文件名:行号:函数名），转发给 spdlog 原生 API
// 用 {} 格式化（fmtlib 风格），类型安全，无缓冲区限制
#define NC_LOGD(...) ::nc::common::getLogger()->log(spdlog::source_loc{__FILE__, __LINE__, __FUNCTION__}, spdlog::level::debug, __VA_ARGS__)
#define NC_LOGI(...) ::nc::common::getLogger()->log(spdlog::source_loc{__FILE__, __LINE__, __FUNCTION__}, spdlog::level::info, __VA_ARGS__)
#define NC_LOGW(...) ::nc::common::getLogger()->log(spdlog::source_loc{__FILE__, __LINE__, __FUNCTION__}, spdlog::level::warn, __VA_ARGS__)
#define NC_LOGE(...) ::nc::common::getLogger()->log(spdlog::source_loc{__FILE__, __LINE__, __FUNCTION__}, spdlog::level::err, __VA_ARGS__)

} // namespace common
} // namespace nc
