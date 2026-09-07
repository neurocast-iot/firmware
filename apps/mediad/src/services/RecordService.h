/**
 * @file RecordService.h
 * @brief 录像服务：命令录像（由 TriggerManager 或 IPC 触发）
 *
 * 录像模式：
 *   - Idle：空闲
 *   - Command：命令触发的一次性录像（duration_sec，0=录到 RECORD_STOP）
 *
 * 录像时机由 TriggerManager 控制（RecordTrigger 发事件），本服务只管“怎么录”。
 *
 * 数据流：subscribe(ChannelId::Main) 帧回调 → Mp4Recorder::writeFrame，
 * 录多久/何时收卷的业务策略全在本服务，Mp4Recorder 只管写文件。
 *
 * 相机重启编排（分辨率热更时由 CameraService 钩子驱动）：
 *   pauseForRestart() 收卷+退订 → 相机重启 → resumeAfterRestart() 恢复
 */
#pragma once

#include "config/ConfigStore.h"

#include "camera/types.h"
#include "camera/event_dispatcher.h"
#include "recorder/mp4_recorder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mediad {

class CameraService;

class RecordService {
public:
    /** 单段文件完成回调（供上层广播 MEDIA_FILE_READY）
     *
     * startTimeSec：这段录像第一帧的墙上时间（秒），服务器用它算文件覆盖范围 */
    using FileReadyCallback = std::function<void(const std::string& triggerType,
                                                 const std::string& path,
                                                 int64_t size, uint64_t durationMs,
                                                 uint64_t startTimeSec,
                                                 const std::string& thumbPath,
                                                 int64_t thumbSize)>;

    /** 录像模式 */
    enum class Mode { Idle, Command };

    explicit RecordService(CameraService& camera) : m_camera(camera) {}
    ~RecordService();

    RecordService(const RecordService&) = delete;
    RecordService& operator=(const RecordService&) = delete;

    void setFileReadyCallback(FileReadyCallback cb);

    /** 保存配置（相机可能未就绪） */
    void start(const RecordCfg& cfg);

    /** 相机启动成功通知 */
    void onCameraStarted();

    /**
     * 应用新配置
     * 分段时长/目录变化时收卷重启录像。
     * 命令录像进行中不受配置影响（录完自然结束）。
     */
    void apply(const RecordCfg& cfg);

    /**
     * 命令录像（IPC RECORD_START）
     * @param durationSec 录像时长（秒），0=录到 RECORD_STOP
     * @return false 时 errMsg 说明原因（busy / camera not running / ...）
     */
    bool startCommand(int durationSec, std::string& errMsg);

    /** 停止命令录像并收卷（IPC RECORD_STOP）；非命令模式时返回 false */
    bool stopCommand(std::string& errMsg);

    /** 相机重启前置：收卷当前文件并退订 */
    void pauseForRestart();

    /** 相机重启完成：恢复录像 */
    void resumeAfterRestart();

    /** 完全停止（进程退出路径）：收卷+退订+停监控线程 */
    void stop();

    Mode mode() const;
    std::string currentFilePath() const;

private:
    /* 内部方法要求调用方已持有 m_mutex */
    bool startRecordingLocked(Mode mode, int durationSec, std::string& errMsg);
    void stopRecordingLocked();

    void durationLoop();   ///< 命令录像时长监控线程

    /* 缩略图守护：录像期间移动侦测抓变化帧，收卷时兜底 */
    void startThumbnailGuard();
    void stopThumbnailGuard();
    void refreshThumbAfterRotate();   ///< 分段切换后把缩略图路径刷成新文件并补抓一张
    void captureMotionThumbnail();
    /* 抓帧 + 写到当前缩略图最终路径，label 只用于日志区分来源（initial/motion/fallback/segment） */
    void saveThumbnailToFile(const char* label);

    CameraService&        m_camera;
    recorder::Mp4Recorder m_recorder;
    FileReadyCallback     m_onFileReady;

    mutable std::mutex m_mutex;
    RecordCfg          m_cfg;
    Mode               m_mode = Mode::Idle;
    camera::SubscriptionId m_subId = camera::kInvalidSubscriptionId;

    /* 命令录像时长控制 */
    std::thread             m_durThread;
    std::atomic<bool>       m_durCancel{false};
    std::mutex              m_durMutex;
    std::condition_variable m_durCv;
    int                     m_durationSec = 0;

    /* 录像缩略图守护：移动侦测 + 第 N 帧初始缩略图 */
    uint64_t                m_startFrameSeq = 0;      ///< 录像开始时的缩略图帧序号
    uint64_t                m_initialFrameSeq = 0;    ///< 第 5 帧目标（m_startFrameSeq + 4）
    bool                    m_initialThumbCaptured = false;  ///< 初始缩略图是否已抓
    bool                    m_motionThumbCaptured = false;   ///< 运动缩略图是否已抓
    std::chrono::steady_clock::time_point m_lastMotionThumbTime;  ///< 上次运动缩略图抓取时间
    std::string             m_currentThumbPath;              ///< 当前缩略图路径（临时）
    std::string             m_currentThumbDestPath;          ///< 当前缩略图最终路径（录像文件名对应）
    std::string             m_guardRecordPath;               ///< 缩略图守护对应的录像文件路径（分段后变了就刷新）
    std::mutex              m_thumbMutex;                    ///< 保护缩略图状态
    std::thread             m_thumbThread;                   ///< 缩略图后台线程（初始帧等待/分段补抓），stop 时 join
};

} // namespace mediad
