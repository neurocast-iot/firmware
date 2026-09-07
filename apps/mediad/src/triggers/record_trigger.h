/**
 * @file record_trigger.h
 * @brief 录像触发源：基于时间段 + 周几自动开始/停止录像
 *
 * 与 TimerTrigger 不同：
 *   - TimerTrigger：周期性触发（每隔 X 秒拍一张）
 *   - RecordTrigger：时间段触发（进入时间段开始录像，离开时间段停止录像）
 *
 * 配置示例：
 *   - 周一到周五 9:00-17:00 录像
 *   - start: 09:00, end: 17:00, weekdays: [1,2,3,4,5]
 */
#pragma once

#include "trigger.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mediad {

/**
 * 录像触发源
 *
 * 监控时间，进入配置的时间段时自动开始录像，离开时间段时自动停止录像。
 * 线程每秒检查一次时间，判断是否在时间段内。
 */
class RecordTrigger : public Trigger {
public:
    struct Config {
        std::string id;
        int priority = 50;
        bool allDay = false;        ///< 全天录像（忽略 start/end 时间）
        int startHour = 0;          ///< 开始小时（0-23）
        int startMin = 0;           ///< 开始分钟（0-59）
        int endHour = 23;           ///< 结束小时（0-23）
        int endMin = 59;            ///< 结束分钟（0-59）
        std::vector<int> weekdays;  ///< 周几生效（0=周日, 1=周一, ..., 6=周六），空=每天
    };

    explicit RecordTrigger(const Config& cfg);
    ~RecordTrigger() override;

    RecordTrigger(const RecordTrigger&) = delete;
    RecordTrigger& operator=(const RecordTrigger&) = delete;

    void enable() override;
    void disable() override;
    bool isEnabled() const override;
    const char* getId() const override { return m_id.c_str(); }

    /** 获取当前配置（用于增量更新时对比） */
    TriggerCfg getConfig() const override;

private:
    /**
     * 时间段监控线程
     *
     * 每秒检查一次时间：
     *   - 进入时间段且未在录 → 触发 Record（TriggerManager 自动开始）
     *   - 离开时间段且在录 → 触发 Record（TriggerManager 自动停止）
     */
    void scheduleLoop();

    /** 判断当前时间是否在配置的时间段内（含周几检查） */
    bool isInTimePeriod() const;

    /** 获取当前周几（0=周日, 1=周一, ..., 6=周六） */
    int getCurrentWeekday() const;

    Config m_cfg;
    std::string m_id;

    std::thread m_thread;
    std::atomic<bool> m_stopFlag{false};
    std::atomic<bool> m_recording{false};  ///< 当前是否在录
    std::mutex m_cvMutex;                  ///< 配合 m_cv 让 disable() 能立即唤醒线程
    std::condition_variable m_cv;
};

} // namespace mediad
