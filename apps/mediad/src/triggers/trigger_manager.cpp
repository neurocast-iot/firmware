/**
 * @file trigger_manager.cpp
 * @brief 触发源管理器实现
 *
 * 线程模型：
 *   - 触发源各自在自己的线程里产生事件
 *   - onTriggered 在触发源线程回调，通过 m_execMutex 串行化执行
 *   - pauseForRestart 会阻塞等待在途动作完成
 */
#include "triggers/trigger_manager.h"

#include "services/RecordService.h"

#include "nc/common/log_utils.h"

#include <cstring>

namespace mediad {

TriggerManager::TriggerManager(SnapshotService& snapshot, RecordService& record)
    : m_snapshot(snapshot), m_record(record) {}

TriggerManager::~TriggerManager() {
    disableAll();
}

void TriggerManager::addTrigger(std::unique_ptr<Trigger> trigger) {
    if (!trigger) return;

    /* 设置回调：触发源触发时通过这个回调把事件交给 TriggerManager，
     * 由 TriggerManager 统一协调执行。
     * 不这样做的话：每个触发源自己执行动作，多个触发源同时拍照会冲突 */
    trigger->onTriggered = [this](const TriggerEvent& event) -> SnapshotResult {
        return onTriggered(event);
    };

    /* 把文件就绪回调传递给每个触发源：
     * TimerTrigger 拍完照直接调它通知上层；
     * BluetoothTrigger 复制完每个标签副本后调它通知 */
    if (m_fileReadyCallback) {
        trigger->setFileReadyCallback(m_fileReadyCallback);
    }

    NC_LOGI("TriggerManager: registered trigger [{}]",
             trigger->getId());
    m_triggers.push_back(std::move(trigger));
}

void TriggerManager::enableTrigger(const char* id) {
    Trigger* t = findTrigger(id);
    if (t) {
        t->enable();
    } else {
        NC_LOGW("TriggerManager: trigger [{}] not found", id);
    }
}

void TriggerManager::disableTrigger(const char* id) {
    Trigger* t = findTrigger(id);
    if (t) {
        t->disable();
    }
}

void TriggerManager::disableAll() {
    for (auto& t : m_triggers) {
        t->disable();
    }
}

void TriggerManager::clearAll() {
    /* 先禁用所有触发源（join 线程），再清空列表 */
    disableAll();
    m_triggers.clear();
    NC_LOGI("TriggerManager: all triggers cleared");
}

bool TriggerManager::removeTrigger(const char* id) {
    /* 先禁用（join 线程），再从列表移除 */
    for (auto it = m_triggers.begin(); it != m_triggers.end(); ++it) {
        if (std::strcmp((*it)->getId(), id) == 0) {
            (*it)->disable();
            m_triggers.erase(it);
            NC_LOGI("TriggerManager: trigger [{}] removed", id);
            return true;
        }
    }
    NC_LOGW("TriggerManager: trigger [{}] not found for removal", id);
    return false;
}

void TriggerManager::pauseForRestart() {
    m_paused.store(true);
    /* 拿一次执行锁：在途动作完成后才返回，保证不再有任何动作在执行。
     * 拿不到锁说明 snapshotNow 还在跑，等它拍完再返回 */
    { std::lock_guard<std::mutex> lock(m_execMutex); }
    NC_LOGI("TriggerManager: paused for camera restart");
}

void TriggerManager::resumeAfterRestart() {
    if (m_paused.exchange(false)) {
        NC_LOGI("TriggerManager: resumed after camera restart");
    }
}

void TriggerManager::setFileReadyCallback(FileReadyCallback cb) {
    m_fileReadyCallback = std::move(cb);
    /* 同步给已注册的所有触发源 */
    for (auto& t : m_triggers) {
        t->setFileReadyCallback(m_fileReadyCallback);
    }
}

SnapshotResult TriggerManager::fireEvent(const TriggerEvent& event) {
    /* 公开的外部触发入口，内部直接转给 onTriggered：
     * 复用串行化、暂停检查、fileReadyCallback 通知等所有机制 */
    return onTriggered(event);
}

SnapshotResult TriggerManager::onTriggered(const TriggerEvent& event) {
    NC_LOGI("TriggerManager: received event from [{}] action={}",
            event.sourceId, static_cast<int>(event.action));

    /* 相机重启窗口内拒绝所有动作：
     * 旧 VI 上下文即将失效，拍照会失败甚至崩溃 */
    if (m_paused.load()) {
        NC_LOGW("TriggerManager: event from [{}] rejected (camera restarting)",
                event.sourceId);
        return SnapshotResult{};
    }

    switch (event.action) {
        case ActionType::Snapshot:
            return executeSnapshot(event);
        case ActionType::Record:
            return executeRecord(event);
    }
    return SnapshotResult{};
}

SnapshotResult TriggerManager::executeSnapshot(const TriggerEvent& event) {
    SnapshotResult result;

    NC_LOGI("TriggerManager: executing snapshot from [{}] priority={}",
            event.sourceId, event.priority);

    /* 优先级抢占检查：如果当前有更高或同等优先级在执行，丢弃新来的。
     * 数字越大优先级越高，例如 SOS=100 > Motion=50 > Bluetooth=30 > Timer=10 */
    int currentPri = m_currentPriority.load();
    if (currentPri >= event.priority) {
        NC_LOGW("TriggerManager: event from [{}] preempted (current priority={} >= {})",
                event.sourceId, currentPri, event.priority);
        return result;
    }

    /* 登记自己的优先级：让正在连拍的低优先级知道有人在等它让位 */

    /* 执行锁：串行化拍照，也让 pauseForRestart 能等到在途拍照结束。
     * 持锁后再查暂停标志，排除“检查时未暂停、拿锁时已进入重启窗口”的竞态 */
    std::lock_guard<std::mutex> lock(m_execMutex);

    if (m_paused.load()) {
        NC_LOGW("TriggerManager: snapshot rejected (camera restarting)");
        return result;
    }

    /* 拿到锁后再检查一次优先级：可能在等锁期间有更高优先级的进来了 */
    currentPri = m_currentPriority.load();
    if (currentPri >= event.priority) {
        NC_LOGW("TriggerManager: event from [{}] preempted after waiting (current priority={} >= {})",
                event.sourceId, currentPri, event.priority);
        return result;
    }

    /* 设置当前执行优先级 */
    m_currentPriority.store(event.priority);

    /* 前缀由各触发源设置，这里直接用 */
    result = m_snapshot.snapshotNow(event.prefix);
    if (!result.ok) {
        NC_LOGW("TriggerManager: snapshot from [{}] failed: {}",
                event.sourceId, result.errMsg.c_str());
    }

    /* 执行完成，重置优先级 */
    m_currentPriority.store(-1);

    return result;
}

SnapshotResult TriggerManager::executeRecord(const TriggerEvent& event) {
    SnapshotResult result;

    /* 根据当前录像状态决定开始还是停止 */
    bool shouldStart = !m_recording.load();

    if (shouldStart) {
        NC_LOGI("TriggerManager: executing record START from [{}] priority={}",
                event.sourceId, event.priority);
    } else {
        NC_LOGI("TriggerManager: executing record STOP from [{}] priority={}",
                event.sourceId, event.priority);
    }

    /* 优先级抢占检查（仅开始录像时检查，停止录像不抢占） */
    if (shouldStart) {
        int currentPri = m_currentPriority.load();
        if (currentPri >= event.priority) {
            NC_LOGW("TriggerManager: record from [{}] preempted (current priority={} >= {})",
                    event.sourceId, currentPri, event.priority);
            return result;
        }
    }

    std::lock_guard<std::mutex> lock(m_execMutex);

    if (m_paused.load()) {
        NC_LOGW("TriggerManager: record from [{}] rejected (camera restarting)",
                event.sourceId);
        return result;
    }

    /* 拿锁后再查一次状态：可能在等锁期间状态变了 */
    shouldStart = !m_recording.load();

    if (shouldStart) {
        /* burstCount 作为录像时长（秒），0=录到手动停止 */
        int durationSec = event.burstCount;
        std::string errMsg;
        if (!m_record.startCommand(durationSec, errMsg)) {
            NC_LOGW("TriggerManager: record start from [{}] failed: {}",
                    event.sourceId, errMsg.c_str());
            result.errMsg = errMsg;
            return result;
        }
        m_recording.store(true);
        NC_LOGI("TriggerManager: record started from [{}] duration={}s",
                event.sourceId, durationSec);
        result.ok = true;
        result.path = m_record.currentFilePath();
    } else {
        std::string errMsg;
        if (!m_record.stopCommand(errMsg)) {
            NC_LOGW("TriggerManager: record stop from [{}] failed: {}",
                    event.sourceId, errMsg.c_str());
            result.errMsg = errMsg;
            return result;
        }
        m_recording.store(false);
        NC_LOGI("TriggerManager: record stopped from [{}]",
                event.sourceId);
        result.ok = true;
    }

    return result;
}

Trigger* TriggerManager::findTrigger(const char* id) {
    for (auto& t : m_triggers) {
        if (std::strcmp(t->getId(), id) == 0) {
            return t.get();
        }
    }
    return nullptr;
}

} // namespace mediad
