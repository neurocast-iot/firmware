/**
 * @file schedule.h
 * @brief 时间表：判断当前时间是否在指定范围内
 *
 * 所有触发源共用：定时拍照、定时录像等都通过 Schedule 判断是否该执行。
 * 没配置时间表 = 全天生效（00:00~23:59，每天）。
 *
 * 资源消耗：~200 字节内存，CPU 忽略不计（几个整数比较）。
 * 和海康/大华 NVR 的 Schedule 功能对齐：每天最多 8 个时间段。
 *
 * 用法：
 *   Schedule sched;
 *   sched.load("10:00", "18:00", {1,2,3,4,5});  // 周一到周五 10:00~18:00
 *   if (sched.isActive()) { ... }                 // 当前时间在范围内？
 */
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace mediad {

/** 单个时间段（如 10:00~18:00） */
struct TimeSlot {
    int startMinutes;   ///< 起始分钟数（从 00:00 起），如 10:00 = 600
    int endMinutes;     ///< 结束分钟数，如 18:00 = 1080
};

/**
 * 时间表
 *
 * 判断当前时间是否在指定的时间段和星期几范围内。
 * 没配置（不调 load）= 全天生效，isActive() 始终返回 true。
 */
class Schedule {
public:
    Schedule() = default;

    /**
     * 从配置加载时间表
     *
     * @param startTime 起始时间字符串，格式 "HH:MM"（如 "10:00"）
     * @param endTime   结束时间字符串，格式 "HH:MM"（如 "18:00"）
     * @param days      生效的星期几（0=周日, 1=周一, ..., 6=周六）
     *
     * @note 解析失败时不配置时间表（全天生效），不报错
     */
    void load(const std::string& startTime, const std::string& endTime,
              const std::vector<int>& days);

    /** 判断当前系统时间是否在时间表内 */
    bool isActive() const;

    /**
     * 判断指定时间是否在时间表内
     *
     * @param now 要判断的时间点（time_t）
     * @return true 表示在时间表内，应该触发
     */
    bool isActive(std::time_t now) const;

    /**
     * 是否配置了时间表
     *
     * 没配置 = 全天生效，isActive() 始终返回 true。
     * 调用方用这个判断是否需要检查时间表：
     *   if (schedule.isConfigured() && !schedule.isActive()) { skip; }
     */
    bool isConfigured() const { return m_configured; }

private:
    /**
     * 解析 "HH:MM" 为分钟数
     *
     * @param timeStr 时间字符串，格式 "HH:MM"（如 "10:30"）
     * @return 分钟数（如 "10:30" → 630），解析失败返回 -1
     */
    static int parseTime(const std::string& timeStr);

    bool m_configured = false;
    std::vector<TimeSlot> m_slots;   ///< 每天的时间段（最多 8 个）
    bool m_days[7] = {};             ///< 星期几生效（0=周日, 6=周六）
};

} // namespace mediad
