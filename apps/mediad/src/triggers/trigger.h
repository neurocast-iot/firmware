/**
 * @file trigger.h
 * @brief 触发源基类与事件定义
 *
 * 所有触发源（定时、蓝牙、SOS、运动检测等）实现这个接口，
 * TriggerManager 统一管理。加新触发源只需要实现这个接口，不动执行代码。
 *
 * 事件流向：
 *   触发源 → onTriggered 回调 → TriggerManager → SnapshotService / RecordService
 *
 * 设计约束：
 *   - TriggerEvent 用固定大小字段（不用 std::string/std::map），嵌入式省内存
 *   - 一个事件 ~60 字节，队列缓存 10 个也才 ~600 字节
 */
#pragma once

#include "config/ConfigStore.h"
#include "services/SnapshotService.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace mediad {

/** 文件就绪回调：通知上层某个文件已生成（供 IPC 广播 MEDIA_FILE_READY） */
using FileReadyCallback = std::function<void(
    const std::string& triggerType,
    const std::string& path,
    int64_t size,
    const std::string& thumbPath,
    int64_t thumbSize)>;

/**
 * 动作类型
 *
 * 前端配置映射：
 *   - "snapshot" → ActionType::Snapshot（拍照）
 *   - "record"   → ActionType::Record（录像，系统自动管开始/停止）
 */
enum class ActionType {
    Snapshot,  ///< 拍照
    Record     ///< 录像（TriggerManager 内部根据当前录像状态决定开始还是停止）
};

/**
 * 触发事件
 *
 * 用固定大小字段（不用 std::string/std::map），嵌入式省内存。
 * 一个事件 ~64 字节，队列缓存 10 个也才 ~640 字节。
 *
 * 各触发源事件字段值：
 *
 * | 字段              | TimerTrigger（定时拍照）    | BluetoothTrigger（蓝牙拍照）   | RecordTrigger（时间段录像）  |
 * |-------------------|---------------------------|------------------------------|---------------------------|
 * | action            | Snapshot                  | Snapshot                     | Record                    |
 * | priority          | 配置值（默认 10）          | 配置值（默认 30）             | 配置值（默认 50）          |
 * | burstCount        | 1（连拍由循环控制）         | 1（连拍由循环控制）           | 0                         |
 * | burstIntervalMs   | 0                         | 0                            | 0                         |
 * | sourceId          | 配置 id                    | 配置 id                       | 配置 id                    |
 * | prefix            | "timer_"                  | "bluetooth_"                 | "record_"                 |
 */
struct TriggerEvent {
    ActionType  action;         ///< 要执行的动作（TriggerManager 靠这个分发）
    int         priority;       ///< 优先级（数字越大越高，用于抢占判断）
    int         burstCount;     ///< 连拍/连录次数（0=录到手动停止）
    int         burstIntervalMs;///< 连拍间隔（毫秒）
    char        sourceId[32];   ///< 触发源 ID（如 "timer_snapshot"）
    char        prefix[16];     ///< 文件名前缀（如 "timer_"、"bluetooth_"）

    TriggerEvent() : action(ActionType::Snapshot),
                     priority(0), burstCount(1), burstIntervalMs(0) {
        sourceId[0] = '\0';
        prefix[0] = '\0';
    }
};

/* 构造 TriggerEvent 的快捷方法：填好固定字段，sourceId/prefix 安全拷贝。
 * 各触发源不用每次手写 6 行 strncpy + 截断保护 */
inline TriggerEvent makeTriggerEvent(ActionType action, int priority,
                                     const char* sourceId, const char* prefix) {
    TriggerEvent e;
    e.action = action;
    e.priority = priority;
    std::strncpy(e.sourceId, sourceId, sizeof(e.sourceId) - 1);
    e.sourceId[sizeof(e.sourceId) - 1] = '\0';
    std::strncpy(e.prefix, prefix, sizeof(e.prefix) - 1);
    e.prefix[sizeof(e.prefix) - 1] = '\0';
    return e;
}

/**
 * 触发源基类
 *
 * 所有触发源实现这个接口：
 *   - enable() / disable()：启用/禁用触发源
 *   - onTriggered：触发时回调，通知 TriggerManager
 *
 * 触发源自己管线程（如 TimerTrigger 的定时线程），
 * 触发时调用 onTriggered 回调把事件交给 TriggerManager。
 *
 * @note onTriggered 由 TriggerManager::addTrigger() 设置，触发源不要自己设置
 */
class Trigger {
public:
    virtual ~Trigger() = default;

    /**
     * 启用触发源
     *
     * 开始监听/定时。幂等：已启用的不会重复创建线程。
     */
    virtual void enable() = 0;

    /**
     * 禁用触发源
     *
     * 停止监听/定时。幂等：已禁用的不会重复 join。
     * 会阻塞等待线程退出。
     */
    virtual void disable() = 0;

    /** 是否已启用 */
    virtual bool isEnabled() const = 0;

    /** 触发源 ID（配置中标识用，如 "timer_snapshot"） */
    virtual const char* getId() const = 0;

    /**
     * 设置文件就绪回调
     *
     * 拍照/复制成功后调用，通知上层文件已生成。
     * 由 TriggerManager 统一设置，触发源不要自己设置。
     */
    void setFileReadyCallback(FileReadyCallback cb) { m_fileReadyCallback = std::move(cb); }

    /**
     * 触发时回调，由 TriggerManager 设置，触发源调用它把事件交出去。
     * 返回 SnapshotResult：ok=true 时 path/size 有效，ok=false 表示失败。
     */
    std::function<SnapshotResult(const TriggerEvent&)> onTriggered;

    /**
     * 获取当前配置（用于增量更新时对比）
     *
     * 返回 TriggerCfg 格式，TriggerManager 用来判断配置是否变了。
     */
    virtual TriggerCfg getConfig() const = 0;

    /**
     * 对比配置是否相同（用于增量更新时判断是否需要重启）
     *
     * 默认实现：逐个字段比较。子类可以覆盖以优化性能。
     */
    virtual bool configEquals(const TriggerCfg& other) const {
        return getConfig() == other;
    }

protected:
    FileReadyCallback m_fileReadyCallback;  ///< 文件就绪回调，通知上层文件已生成
};

} // namespace mediad
