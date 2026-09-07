/**
 * @file signal_manager.cpp
 * @brief 信号管理器实现
 */
#include "signal_manager.h"

#include <csignal>

namespace iot_agent {

SignalManager& SignalManager::instance() {
    static SignalManager inst;
    return inst;
}

/* 信号处理函数：friend 声明在 SignalManager 类里 */
void onExitSignal(int) {
    SignalManager::instance().m_shouldStop.store(true);
}

void onLocalUpgradeSignal(int sig) {
    // SIGUSR1 → SW 升级（1），SIGUSR2 → FW 升级（2）
    SignalManager::instance().m_pendingUpgradeType.store(sig == SIGUSR2 ? 2 : 1);
}

void SignalManager::registerExitHandler() {
    std::signal(SIGTERM, onExitSignal);
    std::signal(SIGINT, onExitSignal);
}

void SignalManager::registerLocalUpgradeHandler() {
    std::signal(SIGUSR1, onLocalUpgradeSignal);
    std::signal(SIGUSR2, onLocalUpgradeSignal);
}

} // namespace iot_agent
