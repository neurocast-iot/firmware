/**
 * @file main.cpp
 * @brief mediad —— NeuroCast 设备媒体能力进程（纯入口）
 *
 * 只做进程级职责：日志初始化、信号处理、生命周期驱动；
 * 服务组装与编排逻辑全部在 App（组装根）中。
 *
 * 用法：mediad [配置文件路径]   默认 /etc/config/mediad.json
 */
#include "App.h"

#include "nc/common/log_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <memory>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

/* 启动环境自检：输出当前时区与本地时间
 * （OSD 时间水印、拍照/录像文件命名都依赖本地时间，
 *   板端时区配错时可从这条日志直接定位） */
void logStartupEnv() {
    std::time_t now = std::time(nullptr);
    std::tm tmBuf;
    localtime_r(&now, &tmBuf);

    char timeStr[32];
    char tzStr[16];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tmBuf);
    std::strftime(tzStr, sizeof(tzStr), "%Z%z", &tmBuf);   /* 如 CST+0800 */
    NC_LOGI("mediad starting... local_time={} timezone={}", timeStr, tzStr);
}

} // namespace

int main(int argc, char* argv[]) {
    /* 日志：控制台 + /mnt/emmc/logs 每日轮转文件（存储未挂载时自动降级纯控制台） */
    nc::common::LogInit("/mnt/emmc/logs/mediad.log", nc::common::LogLevel::kInfo);
    logStartupEnv();

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);   /* IPC 对端异常断开不杀进程 */

    const std::string configPath = (argc > 1) ? argv[1] : "/etc/config/mediad.json";

    /* App 放堆上：嵌入式设备内存紧张，放堆上避免栈溢出 */
    std::unique_ptr<mediad::App> app(new mediad::App());
    app->init(configPath);

    /* 主循环：等退出信号（SIGINT/SIGTERM 优雅退出） */
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    app->shutdown();
    return 0;
}
