/**
 * @file timer_trigger.h
 * @brief 定时触发源：按固定间隔触发
 *
 * 替代原来 SnapshotService::scheduleLoop() 的定时逻辑，
 * 增加时间表支持：只在 Schedule 指定的时间段内触发。
 *
 * 一个 TimerTrigger 可以触发拍照或录像，由 action 字段决定。
 * 配置热更：间隔/时间表变了立即生效（沿用 SnapshotService 的配置变更计数模式）。
 *
 * 线程模型：
 *   - enable() 创建定时线程，disable() join 回收
 *   - 定时线程 sleep → 检查时间表 → 触发回调
 */
#pragma once

#include "trigger.h"
#include "schedule.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace mediad {

/**
 * 定时触发源
 *
 * 按固定间隔触发，支持时间表约束（只在指定时间段内触发）。
 * 配置热更通过 applyConfig() 实现，间隔变了立即生效。
 */
class TimerTrigger : public Trigger {
public:
    /** 定时触发源配置 */
    struct Config {
        std::string id;
        bool enabled = false;
        int priority = 0;                   ///< 优先级（数字越大越高）
        int intervalSec = 300;              ///< 触发间隔（秒）
        int burstCount = 1;                 ///< 连拍次数
        int burstIntervalMs = 0;            ///< 连拍间隔（毫秒）
        bool allDay = false;                ///< 全天生效（忽略 schedule）
        Schedule schedule;                  ///< 时间表（allDay=false 时使用）
        /* 原始 schedule 字段（用于 getConfig() 返回给 TriggerManager 对比） */
        std::string scheduleStart;          ///< 起始时间（如 "10:00"）
        std::string scheduleEnd;            ///< 结束时间（如 "18:00"）
        std::vector<int> scheduleDays;      ///< 生效的星期几
    };

    explicit TimerTrigger(const Config& cfg);
    ~TimerTrigger() override;

    TimerTrigger(const TimerTrigger&) = delete;
    TimerTrigger& operator=(const TimerTrigger&) = delete;

    /**
     * 启用触发源：创建定时线程
     *
     * 幂等：已在跑的不会重复创建线程。
     * 线程启动后立即进入定时循环。
     */
    void enable() override;

    /**
     * 禁用触发源：停止定时线程
     *
     * 幂等：已停止的不会重复 join。
     * 会阻塞等待线程退出（最多一个间隔周期）。
     */
    void disable() override;

    bool isEnabled() const override;

    /**
     * 热更配置
     *
     * 间隔/时间表变了立即生效：叫醒定时线程重读新配置。
     * 不这样做的话：改间隔要等旧周期睡完才生效。
     */
    void applyConfig(const Config& cfg);

    const char* getId() const override { return m_id.c_str(); }

    /** 获取当前配置（用于增量更新时对比） */
    TriggerCfg getConfig() const override;

private:
    /**
     * 定时线程主循环
     *
     * sleep → 检查配置是否变更 → 检查时间表 → 触发回调。
     * 配置变更通过 m_cfgGen 计数检测，睡前记下计数，醒来对不上就知道配置改过了。
     */
    void timerLoop();

    std::string m_id;
    Config m_cfg;
    mutable std::mutex m_cfgMutex;

    std::thread m_thread;
    std::atomic<bool> m_stopFlag{false};
    std::mutex m_cvMutex;
    std::condition_variable m_cv;
    /* 配置变更计数：applyConfig 每改一次就加 1。
     * 定时线程睡前记下计数，醒来对不上就知道配置改过了，重读新间隔重新睡。
     * 沿用 SnapshotService 的成熟模式 */
    std::atomic<uint64_t> m_cfgGen{0};
};

} // namespace mediad
