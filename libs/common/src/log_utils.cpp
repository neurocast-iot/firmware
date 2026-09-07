// nc::common 日志工具实现（spdlog 后端）
//
// spdlog 是实现细节，只出现在本 .cpp：全仓调用方只认 NC_LOGx 宏，
// 将来换日志库只动本文件。配置对齐 gw_av100 LogUtils 的验证过的方案：
//   - 异步日志器（队列 8192 + 1 个后台线程，满时阻塞不丢日志）
//   - 每日轮转文件 sink（00:00 轮转，追加模式，保留 7 天）
//   - INFO 及以上逐条刷盘（保证 tail -f 实时可见）
#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <mutex>
#include <vector>

namespace nc {
namespace common {

namespace {

std::mutex g_init_mutex;
std::shared_ptr<spdlog::logger> g_logger;

spdlog::level::level_enum toSpdLevel(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return spdlog::level::debug;
        case LogLevel::kInfo:  return spdlog::level::info;
        case LogLevel::kWarn:  return spdlog::level::warn;
        case LogLevel::kError: return spdlog::level::err;
    }
    return spdlog::level::info;
}

/* 用给定 sink 组装异步日志器并设为全局（调用方需持 g_init_mutex） */
void setupLogger(std::vector<spdlog::sink_ptr> sinks, LogLevel min_level) {
    static std::once_flag pool_once;
    std::call_once(pool_once, [] {
        spdlog::init_thread_pool(8192, 1);
    });

    g_logger = std::make_shared<spdlog::async_logger>(
        "nc", sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#:%!] %v");
    g_logger->set_level(toSpdLevel(min_level));
    /* INFO 及以上立即刷盘，掉电/崩溃最多丢 DEBUG 级缓冲 */
    g_logger->flush_on(spdlog::level::info);
}

/* LogInit 之前就有日志调用时的兜底：先建一个纯控制台日志器 */
std::shared_ptr<spdlog::logger> ensureLogger() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (!g_logger) {
        setupLogger({std::make_shared<spdlog::sinks::stdout_color_sink_mt>()},
                    LogLevel::kInfo);
    }
    return g_logger;
}

} // namespace

void LogInit(const std::string& log_path, LogLevel min_level) {
    std::lock_guard<std::mutex> lock(g_init_mutex);

    /* 控制台 sink 始终保留（现场串口/SSH 直接可见） */
    std::vector<spdlog::sink_ptr> sinks = {
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};

    if (!log_path.empty()) {
        /* 日志目录不存在则先创建（如 /mnt/emmc/logs 首次启动） */
        size_t slash = log_path.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            MakeDirs(log_path.substr(0, slash));
        }
        try {
            /* 每日 00:00 轮转，追加模式，保留 7 天 */
            sinks.push_back(std::make_shared<spdlog::sinks::daily_file_sink_mt>(
                log_path, 0, 0, false, 7));
        } catch (const std::exception& e) {
            /* 存储未挂载等场景：降级纯控制台，进程不因日志挂掉 */
            std::fprintf(stderr, "LogInit: file sink failed (%s), console only\n",
                         e.what());
        }
    }

    setupLogger(std::move(sinks), min_level);
}

std::shared_ptr<spdlog::logger> getLogger() {
    return ensureLogger();
}

} // namespace common
} // namespace nc
