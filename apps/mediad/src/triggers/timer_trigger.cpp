/**
 * @file timer_trigger.cpp
 * @brief 定时触发源实现
 *
 * 定时线程主循环：sleep → 检查配置变更 → 检查时间表 → 触发回调。
 * 配置变更通过 m_cfgGen 计数检测，保证热更立即生效。
 */
#include "triggers/timer_trigger.h"

#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace mediad {

TimerTrigger::TimerTrigger(const Config& cfg) : m_cfg(cfg), m_id(cfg.id) {}

TimerTrigger::~TimerTrigger() {
    disable();
}

void TimerTrigger::enable() {
    if (m_thread.joinable()) {
        return;   /* 已在跑，不重复创建 */
    }
    m_stopFlag = false;
    m_thread = std::thread(&TimerTrigger::timerLoop, this);
    NC_LOGI("TimerTrigger[{}]: enabled (interval={}s)", m_id.c_str(), m_cfg.intervalSec);
}

void TimerTrigger::disable() {
    if (!m_thread.joinable()) {
        return;   /* 本来就没开 */
    }
    m_stopFlag = true;
    m_cv.notify_all();
    m_thread.join();
    NC_LOGI("TimerTrigger[{}]: disabled", m_id.c_str());
}

bool TimerTrigger::isEnabled() const {
    return m_thread.joinable();
}

void TimerTrigger::applyConfig(const Config& cfg) {
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        m_cfg = cfg;
    }
    /* 配置变更计数加 1，叫醒定时线程重读新间隔。
     * 不这样做的话：改间隔要等旧周期睡完才生效 */
    m_cfgGen.fetch_add(1);
    m_cv.notify_all();
    NC_LOGI("TimerTrigger[{}]: config applied (interval={}s)",
            m_id.c_str(), cfg.intervalSec);
}

void TimerTrigger::timerLoop() {
    while (!m_stopFlag) {
        int intervalSec;
        Schedule schedule;
        {
            std::lock_guard<std::mutex> lock(m_cfgMutex);
            intervalSec = m_cfg.intervalSec;
            schedule = m_cfg.schedule;
        }

        /* 记下睡前的配置变更计数：睡着后谁改了配置，计数对不上就会被叫醒。
         * wait_for 的第三个参数是谓词，返回 true 时提前醒来 */
        const uint64_t genAtWait = m_cfgGen.load();
        bool cfgChanged;
        {
            std::unique_lock<std::mutex> lock(m_cvMutex);
            cfgChanged = m_cv.wait_for(lock, std::chrono::seconds(intervalSec),
                [this, genAtWait] {
                    return m_stopFlag.load() || m_cfgGen.load() != genAtWait;
                });
        }
        if (m_stopFlag) {
            break;
        }
        /* 配置改了：手里的 interval 是睡前读的旧值，不触发，
         * 回到循环开头重新读最新间隔 */
        if (cfgChanged) {
            continue;
        }

        /* 时间表检查：allDay=true 时跳过，否则检查时间表。
         * 没配置时间表 = 全天生效，isActive() 始终返回 true */
        bool isAllDay;
        {
            std::lock_guard<std::mutex> lock(m_cfgMutex);
            isAllDay = m_cfg.allDay;
        }
        if (!isAllDay && schedule.isConfigured() && !schedule.isActive()) {
            continue;
        }

        /* 触发事件：构造 TriggerEvent 并回调 TriggerManager。
         * TriggerManager 现在只拍一张就返回，连拍循环由 TimerTrigger 自己管理 */
        if (onTriggered) {
            int burst;
            int intervalMs;
            {
                std::lock_guard<std::mutex> lock(m_cfgMutex);
                burst = m_cfg.burstCount;
                intervalMs = m_cfg.burstIntervalMs;
            }

            for (int i = 0; i < burst; ++i) {
                TriggerEvent event = makeTriggerEvent(
                    ActionType::Snapshot, m_cfg.priority,
                    m_id.c_str(), "timer_");
                {
                    std::lock_guard<std::mutex> lock(m_cfgMutex);
                    event.burstCount = 1;  /* 每次只拍一张 */
                }

                auto result = onTriggered(event);
                if (!result.ok) {
                    NC_LOGW("TimerTrigger[{}]: snapshot {}/{} failed",
                            m_id.c_str(), i + 1, burst);
                } else {
                    NC_LOGI("TimerTrigger[{}]: snapshot {}/{} ok: {}",
                            m_id.c_str(), i + 1, burst, result.path.c_str());
                    /* 通知上层文件已生成（含缩略图路径） */
                    if (m_fileReadyCallback) {
                        m_fileReadyCallback("timer", result.path, result.size,
                                           result.thumbPath, result.thumbSize);
                    }
                }

                /* 连拍间隔：最后一张不睡 */
                if (i < burst - 1 && intervalMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                }
            }
        } else {
            /* 正常流程不会走到这：addTrigger 一定会设置回调。
             * 打 warning 是为了排查配置/初始化顺序问题 */
            NC_LOGW("TimerTrigger[{}]: onTriggered callback not set!", m_id.c_str());
        }
    }
}

TriggerCfg TimerTrigger::getConfig() const {
    std::lock_guard<std::mutex> lock(m_cfgMutex);
    TriggerCfg cfg;
    cfg.id = m_id;
    cfg.type = "timer";
    cfg.enabled = isEnabled();
    cfg.priority = m_cfg.priority;
    cfg.intervalSec = m_cfg.intervalSec;
    cfg.burstCount = m_cfg.burstCount;
    cfg.burstIntervalMs = m_cfg.burstIntervalMs;
    cfg.allDay = m_cfg.allDay;
    /* schedule 字段从 m_cfg 的原始字段获取 */
    cfg.scheduleStart = m_cfg.scheduleStart;
    cfg.scheduleEnd = m_cfg.scheduleEnd;
    cfg.scheduleDays = m_cfg.scheduleDays;
    return cfg;
}

} // namespace mediad
