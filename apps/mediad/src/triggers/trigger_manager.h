/**
 * @file trigger_manager.h
 * @brief 触发源管理器
 *
 * 职责：
 *   1. 持有所有触发源（TimerTrigger / BtTrigger / ...）
 *   2. 接收触发事件，协调冲突（同一时间只执行一个拍照动作）
 *   3. 调用 SnapshotService / RecordService 执行动作
 *
 * 不持有线程：触发源各自的线程产生事件，这里只是分发和执行。
 *
 * 线程安全：
 *   - addTrigger/enableTrigger/disableTrigger 在主线程调用
 *   - onTriggered 在触发源线程回调，通过 m_execMutex 串行化
 *   - pauseForRestart 在相机重启线程调用，会阻塞等待在途动作完成
 */
#pragma once

#include "trigger.h"
#include "services/SnapshotService.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mediad {

class SnapshotService;
class RecordService;

/**
 * 触发源管理器
 *
 * 所有触发源通过 addTrigger() 注册，enableTrigger/disableTrigger 控制开关。
 * 触发事件统一走 onTriggered 回调，由 TriggerManager 协调执行。
 *
 * 优先级抢占：
 *   - 每个触发事件带优先级（数字越大越高）
 *   - 高优先级事件可以抢占低优先级正在执行的动作
 *   - 被抢占的动作直接丢弃（不排队）
 */
class TriggerManager {
public:
    TriggerManager(SnapshotService& snapshot, RecordService& record);
    ~TriggerManager();

    TriggerManager(const TriggerManager&) = delete;
    TriggerManager& operator=(const TriggerManager&) = delete;

    /**
     * 注册触发源并设置回调
     *
     * 触发源触发时通过这个回调把事件交给 TriggerManager，
     * 由 TriggerManager 统一协调执行，避免多个触发源同时拍照冲突。
     *
     * @note 调用后触发源的 ownership 转移给 TriggerManager
     */
    void addTrigger(std::unique_ptr<Trigger> trigger);

    /** 按 ID 启用触发源（幂等：已启用的不会重复创建线程） */
    void enableTrigger(const char* id);

    /** 按 ID 禁用触发源（幂等：已禁用的不会重复 join） */
    void disableTrigger(const char* id);

    /**
     * 禁用所有触发源
     *
     * 进程退出路径调用，确保所有触发源线程都已 join。
     */
    void disableAll();

    /**
     * 清空所有触发源
     *
     * 配置热更时调用：先禁用所有触发源，再清空列表，
     * 然后根据新配置重新创建。
     */
    void clearAll();

    /**
     * 按 ID 移除触发源
     *
     * 增量更新时使用：配置变了的触发源先移除再重新创建。
     * @return true=成功移除，false=找不到该 ID
     */
    bool removeTrigger(const char* id);

    /**
     * 相机重启前排空在途动作
     *
     * 设置暂停标志后拿一次执行锁：在途动作完成后才返回，
     * 保证返回时不再有任何动作在执行，相机重启可安全进行。
     *
     * @note 会阻塞，直到在途动作完成（百毫秒级）
     */
    void pauseForRestart();

    /** 相机重启完成后恢复（幂等：正常启动路径调用无副作用） */
    void resumeAfterRestart();

    /**
     * 设置文件就绪回调
     *
     * 每次拍照成功后调用，通知上层文件已生成（供 IPC 广播）。
     * 蓝牙触发源也通过这个回调通知每个标签的副本文件。
     */
    void setFileReadyCallback(FileReadyCallback cb);

    /**
     * 外部触发事件（供 IPC 命令等非触发源路径使用）
     *
     * 比如 IPC 收到 MEDIA_SNAPSHOT 后构造一个 Snapshot 事件传进来，
     * 走跟触发源完全一样的通路：串行化执行 + fileReadyCallback 通知。
     * 走这里而不是直接调 SnapshotService 的好处：
     *   1. 跟 timer/bt trigger 统一，路径一致
     *   2. 拍完自动调 fileReadyCallback，iot_agent 能收到 MEDIA_FILE_READY
     *   3. 相机重启期间被 pauseForRestart 拦截，不会崩溃
     */
    SnapshotResult fireEvent(const TriggerEvent& event);

private:
    /**
     * 触发事件分发入口
     *
     * 触发源线程回调，根据动作类型分发到具体执行函数。
     * 相机重启窗口内拒绝所有动作。
     * @return 拍照成功返回 SnapshotResult（ok=true），失败返回 ok=false
     */
    SnapshotResult onTriggered(const TriggerEvent& event);

    /**
     * 执行单次拍照
     *
     * 通过 m_execMutex 串行化拍照请求，拍一张就返回结果。
     * 连拍循环由各触发源自己管理。
     */
    SnapshotResult executeSnapshot(const TriggerEvent& event);

    /**
     * 执行录像动作
     *
     * 内部维护录像状态：当前没在录就开始，正在录就停止。
     * 通过 m_execMutex 串行化，避免与拍照冲突。
     */
    SnapshotResult executeRecord(const TriggerEvent& event);

    /** 按 ID 查找触发源，找不到返回 nullptr */
    Trigger* findTrigger(const char* id);

    SnapshotService& m_snapshot;
    RecordService& m_record;
    std::vector<std::unique_ptr<Trigger>> m_triggers;
    std::mutex m_execMutex;              ///< 动作执行锁（同一时间只拍一张）
    std::atomic<bool> m_paused{false};   ///< 相机重启窗口内拒绝动作
    std::atomic<int> m_currentPriority{-1};  ///< 当前执行的动作优先级（用于抢占判断）
    std::atomic<bool> m_recording{false};    ///< 当前是否在录像（Record 事件靠这个判断开始还是停止）
    FileReadyCallback m_fileReadyCallback;      ///< 文件就绪回调（拍照/复制成功后通知上层）
};

} // namespace mediad
