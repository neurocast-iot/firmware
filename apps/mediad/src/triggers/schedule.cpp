/**
 * @file schedule.cpp
 * @brief 时间表实现
 *
 * 核心逻辑：把时间转成分钟数，判断是否在 [startMinutes, endMinutes) 范围内。
 * 没配置时间表 = 全天生效。
 */
#include "triggers/schedule.h"

#include <cstdlib>
#include <cstring>

namespace mediad {

void Schedule::load(const std::string& startTime, const std::string& endTime,
                    const std::vector<int>& days) {
    /* 清空旧数据：load 可能被多次调用（配置热更） */
    m_slots.clear();
    std::memset(m_days, 0, sizeof(m_days));

    /* 解析时间段 */
    int startMin = parseTime(startTime);
    int endMin = parseTime(endTime);
    if (startMin < 0 || endMin < 0) {
        /* 解析失败，不配置时间表（全天生效）。
         * 不报错是因为配置可能只传了部分字段，兜底走全天 */
        m_configured = false;
        return;
    }

    TimeSlot slot;
    slot.startMinutes = startMin;
    slot.endMinutes = endMin;
    m_slots.push_back(slot);

    /* 解析星期几：days 数组里的值转成 m_days 布尔数组 */
    for (int day : days) {
        if (day >= 0 && day <= 6) {
            m_days[day] = true;
        }
        /* 越界的值忽略：防止配置错误导致数组越界 */
    }

    m_configured = true;
}

bool Schedule::isActive() const {
    return isActive(std::time(nullptr));
}

bool Schedule::isActive(std::time_t now) const {
    /* 没配置时间表 = 全天生效 */
    if (!m_configured) {
        return true;
    }

    /* 转本地时间 */
    std::tm tmBuf;
    localtime_r(&now, &tmBuf);

    /* 检查星期几：不在生效的星期几里就跳过 */
    if (!m_days[tmBuf.tm_wday]) {
        return false;
    }

    /* 计算当前时间的分钟数（从 00:00 起） */
    int currentMinutes = tmBuf.tm_hour * 60 + tmBuf.tm_min;

    /* 检查是否在某个时间段内：左闭右开 [start, end) */
    for (const auto& slot : m_slots) {
        if (currentMinutes >= slot.startMinutes &&
            currentMinutes < slot.endMinutes) {
            return true;
        }
    }

    return false;
}

int Schedule::parseTime(const std::string& timeStr) {
    /* 格式 "HH:MM"，如 "10:30"。
     * 至少 4 个字符，冒号在第 1 或第 2 位 */
    if (timeStr.size() < 4 || timeStr[1] != ':' && timeStr[2] != ':') {
        return -1;
    }

    /* 找冒号位置 */
    size_t colonPos = timeStr.find(':');
    if (colonPos == std::string::npos || colonPos == 0 || colonPos >= timeStr.size() - 1) {
        return -1;
    }

    int hour = std::atoi(timeStr.substr(0, colonPos).c_str());
    int minute = std::atoi(timeStr.substr(colonPos + 1).c_str());

    /* 范围检查：小时 0~23，分钟 0~59 */
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return -1;
    }

    return hour * 60 + minute;
}

} // namespace mediad
