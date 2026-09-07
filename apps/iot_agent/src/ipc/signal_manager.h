/**
 * @file signal_manager.h
 * @brief 信号管理器 —— 统一注册所有信号处理
 *
 * 为什么需要统一信号管理：
 *   - 避免信号处理分散在多个模块
 *   - 避免信号冲突（同一信号只能有一个处理函数）
 *   - 集中管理，便于调试和维护
 *
 * 支持的信号：
 *   - SIGTERM / SIGINT → 退出标记（g_shouldStop）
 *   - SIGUSR1 → SW 本地升级
 *   - SIGUSR2 → FW 本地升级
 */
#pragma once

#include <atomic>
#include <functional>

namespace iot_agent {

class SignalManager {
public:
    /** 获取单例 */
    static SignalManager& instance();

    /** 注册退出信号处理（SIGTERM / SIGINT） */
    void registerExitHandler();

    /** 注册本地升级信号处理（SIGUSR1 / SIGUSR2） */
    void registerLocalUpgradeHandler();

    /** 是否收到退出信号 */
    bool shouldStop() const { return m_shouldStop.load(); }

    /** 获取待处理的本地升级类型：0=无，1=sw，2=fw */
    int pendingUpgradeType() const { return m_pendingUpgradeType.load(); }

    /** 清除待处理的本地升级类型 */
    void clearPendingUpgrade() { m_pendingUpgradeType.store(0); }

private:
    SignalManager() = default;
    ~SignalManager() = default;
    SignalManager(const SignalManager&) = delete;
    SignalManager& operator=(const SignalManager&) = delete;

    /* 信号处理函数需要访问私有成员 */
    friend void onExitSignal(int);
    friend void onLocalUpgradeSignal(int);

    std::atomic<bool> m_shouldStop{false};
    std::atomic<int> m_pendingUpgradeType{0};
};

} // namespace iot_agent
