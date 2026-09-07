/**
 * @file record_trigger.cpp
 * @brief 录像触发源实现
 *
 * 时间段监控线程：每秒检查时间，进入时间段开始录像，离开时间段停止录像。
 */
#include "triggers/record_trigger.h"

#include "nc/common/log_utils.h"

#include <chrono>
#include <ctime>

namespace mediad {

RecordTrigger::RecordTrigger(const Config& cfg) : m_cfg(cfg), m_id(cfg.id) {}

RecordTrigger::~RecordTrigger() {
    disable();
}

void RecordTrigger::enable() {
    if (m_thread.joinable()) {
        return;   /* 已在跑，不重复创建 */
    }
    m_stopFlag = false;
    m_thread = std::thread(&RecordTrigger::scheduleLoop, this);
    NC_LOGI("RecordTrigger[{}]: enabled (start={:02d}:{:02d} end={:02d}:{:02d} weekdays={})",
            m_id.c_str(), m_cfg.startHour, m_cfg.startMin,
            m_cfg.endHour, m_cfg.endMin, m_cfg.weekdays.size());
}

void RecordTrigger::disable() {
    if (!m_thread.joinable()) {
        return;   /* 本来就没开 */
    }
    m_stopFlag = true;
    m_cv.notify_all();   /* 叫醒监控线程，不用等 1 秒睡完 */
    m_thread.join();

    /* 如果在录，发一个 Record 事件让 TriggerManager 停录像 */
    if (m_recording.load() && onTriggered) {
        TriggerEvent event = makeTriggerEvent(
            ActionType::Record, m_cfg.priority,
            m_id.c_str(), "record_");
        event.burstCount = 0;
        onTriggered(event);
        m_recording.store(false);
    }

    NC_LOGI("RecordTrigger[{}]: disabled", m_id.c_str());
}

bool RecordTrigger::isEnabled() const {
    return m_thread.joinable();
}

/**
 * 时间段监控线程主循环
 *
 * 每秒检查一次时间：
 *   - 在时间段内且未在录 → 触发 Record（TriggerManager 自动开始）
 *   - 不在时间段内且在录 → 触发 Record（TriggerManager 自动停止）
 */
void RecordTrigger::scheduleLoop() {
    while (!m_stopFlag.load()) {
        bool inPeriod = isInTimePeriod();
        bool wasRecording = m_recording.load();

        if (inPeriod && !wasRecording) {
            /* 进入时间段，开始录像 */
            if (onTriggered) {
                TriggerEvent event = makeTriggerEvent(
                    ActionType::Record, m_cfg.priority,
                    m_id.c_str(), "record_");
                event.burstCount = 0;  /* 0=录到时间段结束 */

                auto result = onTriggered(event);
                if (result.ok) {
                    m_recording.store(true);
                    NC_LOGI("RecordTrigger[{}]: recording started at {:02d}:{:02d}",
                            m_id.c_str(), m_cfg.startHour, m_cfg.startMin);
                } else {
                    NC_LOGW("RecordTrigger[{}]: recording start failed: {}",
                            m_id.c_str(), result.errMsg.c_str());
                }
            }
        } else if (!inPeriod && wasRecording) {
            /* 离开时间段，停止录像 */
            if (onTriggered) {
                TriggerEvent event = makeTriggerEvent(
                    ActionType::Record, m_cfg.priority,
                    m_id.c_str(), "record_");
                event.burstCount = 0;

                auto result = onTriggered(event);
                if (result.ok) {
                    m_recording.store(false);
                    NC_LOGI("RecordTrigger[{}]: recording stopped at {:02d}:{:02d}",
                            m_id.c_str(), m_cfg.endHour, m_cfg.endMin);
                } else {
                    NC_LOGW("RecordTrigger[{}]: recording stop failed: {}",
                            m_id.c_str(), result.errMsg.c_str());
                }
            }
        }

        /* 每秒检查一次时间（用条件变量等，disable 能立即叫醒） */
        {
            std::unique_lock<std::mutex> lock(m_cvMutex);
            m_cv.wait_for(lock, std::chrono::seconds(1),
                          [this] { return m_stopFlag.load(); });
        }
    }
}

/**
 * 判断当前时间是否在配置的时间段内
 *
 * 检查逻辑：
 *   1. 检查周几是否在 weekdays 列表中（空列表=每天）
 *   2. allDay=true 时直接返回 true
 *   3. 否则判断是否在 [start, end] 范围内
 */
bool RecordTrigger::isInTimePeriod() const {
    /* 检查周几 */
    if (!m_cfg.weekdays.empty()) {
        int weekday = getCurrentWeekday();
        bool weekdayMatch = false;
        for (int wd : m_cfg.weekdays) {
            if (wd == weekday) {
                weekdayMatch = true;
                break;
            }
        }
        if (!weekdayMatch) {
            return false;
        }
    }

    /* 全天录像：只要周几匹配就直接返回 true */
    if (m_cfg.allDay) {
        return true;
    }

    /* 获取当前时间 */
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    localtime_r(&tt, &tmBuf);

    int currentMinutes = tmBuf.tm_hour * 60 + tmBuf.tm_min;
    int startMinutes = m_cfg.startHour * 60 + m_cfg.startMin;
    int endMinutes = m_cfg.endHour * 60 + m_cfg.endMin;

    /* 判断是否在时间段内 */
    if (startMinutes <= endMinutes) {
        /* 正常情况：start <= end，例如 09:00-17:00 */
        return currentMinutes >= startMinutes && currentMinutes <= endMinutes;
    } else {
        /* 跨午夜：start > end，例如 22:00-06:00 */
        return currentMinutes >= startMinutes || currentMinutes <= endMinutes;
    }
}

/**
 * 获取当前周几
 *
 * @return 0=周日, 1=周一, ..., 6=周六
 */
int RecordTrigger::getCurrentWeekday() const {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    localtime_r(&tt, &tmBuf);
    /* tm_wday: 0=周日, 1=周一, ..., 6=周六 */
    return tmBuf.tm_wday;
}

TriggerCfg RecordTrigger::getConfig() const {
    TriggerCfg cfg;
    cfg.id = m_id;
    cfg.type = "record";
    cfg.enabled = isEnabled();
    cfg.priority = m_cfg.priority;
    cfg.allDay = m_cfg.allDay;
    /* 把 startHour/startMin 转换成 scheduleStart 字符串 */
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", m_cfg.startHour, m_cfg.startMin);
    cfg.scheduleStart = buf;
    std::snprintf(buf, sizeof(buf), "%02d:%02d", m_cfg.endHour, m_cfg.endMin);
    cfg.scheduleEnd = buf;
    cfg.scheduleDays = m_cfg.weekdays;
    return cfg;
}

} // namespace mediad
