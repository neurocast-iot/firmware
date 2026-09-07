/**
 * @file main.cpp
 * @brief iot_agent —— NeuroCast 设备云端通信进程（纯入口）
 *
 * 只做进程级职责：日志初始化、信号处理、生命周期驱动；
 * 服务组装与编排逻辑全部在 App（组装根）中。
 *
 * 用法：iot_agent [配置文件路径]   默认 /etc/config/iot_agent.json
 */
#include "App.h"
#include "ipc/signal_manager.h"

#include "nc/common/log_utils.h"

#include <memory>
#include <string>
#include <unistd.h>

int main(int argc, char* argv[]) {
    /* 日志：控制台 + /mnt/emmc/logs 每日轮转文件 */
    nc::common::LogInit("/mnt/emmc/logs/iot_agent.log", nc::common::LogLevel::kInfo);
    NC_LOGI("iot_agent starting");

    /* 统一注册所有信号处理（退出 + 本地升级） */
    auto& signalMgr = iot_agent::SignalManager::instance();
    signalMgr.registerExitHandler();
    signalMgr.registerLocalUpgradeHandler();

    const std::string configPath = (argc > 1) ? argv[1] : "/etc/config/iot_agent.json";

    /* App 放堆上：嵌入式设备内存紧张，放堆上避免栈溢出 */
    std::unique_ptr<iot_agent::App> app(new iot_agent::App());
    int rc = app->init(configPath);
    if (rc != 0) {
        NC_LOGE("iot_agent init failed (rc={})", rc);
        return rc;
    }

    /* 主循环：等退出信号（SIGINT/SIGTERM 优雅退出） */
    while (!signalMgr.shouldStop()) {
        app->pollLocalUpgrade();
        ::sleep(1);
    }

    app->shutdown();
    return 0;
}
