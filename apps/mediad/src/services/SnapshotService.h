/**
 * @file SnapshotService.h
 * @brief 拍照服务：纯执行器（不负责定时，定时由 TriggerManager 驱动）
 *
 * 拍照统一走子通道（ChannelId::Sub）：与主通道录像/推流隔离，
 * 抓帧编码 JPEG 不干扰主码流（快照子通道隔离规则）。
 *
 * 触发源归一：TriggerManager（定时/蓝牙/SOS/手动）
 * 最终都调用 snapshotNow()，行为完全一致。
 *
 * 相机重启编排（分辨率热更时由 CameraService 钩子驱动）：
 *   pauseForRestart() 排空在途拍照并拒绝新请求 → 相机重启 →
 *   resumeAfterRestart() 解除暂停
 *
 * 文件路径：outputDir/yyyyMMdd/HH/{prefix}yyyyMMdd_HHMMSS{ms}.jpg
 */
#pragma once

#include "config/ConfigStore.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace mediad {

class CameraService;

/**
 * 拍照结果
 */
struct SnapshotResult {
    std::string path;      ///< 成功时的原图文件路径
    std::string thumbPath; ///< 成功时的缩略图文件路径（空 = 未生成缩略图）
    int64_t     size = 0;  ///< 成功时的原图文件大小（字节）
    int64_t     thumbSize = 0;  ///< 缩略图文件大小（字节）
    std::string errMsg;    ///< 失败原因，成功时为空
    bool        ok = false;
};

/**
 * 拍照服务（纯执行器）
 *
 * 职责：
 *   - 存储拍照配置（输出目录、JPEG 质量）
 *   - 执行 snapshotNow()（被 TriggerManager 调用）
 *   - 相机重启时暂停/恢复
 *
 * 不负责定时：定时逻辑由 TriggerManager + TimerTrigger 处理。
 */
class SnapshotService {
public:
    explicit SnapshotService(CameraService& camera) : m_camera(camera) {}
    ~SnapshotService() = default;

    SnapshotService(const SnapshotService&) = delete;
    SnapshotService& operator=(const SnapshotService&) = delete;

    /** 启动服务：存储配置（不创建线程） */
    void start(const SnapshotCfg& cfg);

    /** 应用新配置（只更新输出目录/质量，不影响定时） */
    void apply(const SnapshotCfg& cfg);

    /** 停止服务（幂等，当前无资源需释放） */
    void stop();

    /**
     * 相机重启前置：拒绝新拍照请求，并等待在途拍照完成后才返回
     * （返回即保证不再有任何线程触碰相机 SDK）
     */
    void pauseForRestart();

    /** 相机重启完成：解除暂停（幂等，正常启动路径调用无副作用） */
    void resumeAfterRestart();

    /**
     * 立即拍照（TriggerManager 调用入口）
     *
     * quality 和 outputDir 从内部配置读取，调用方只需传文件名前缀。
     *
     * @param prefix  文件名前缀（如 "timer_"、"bluetooth_"），区分触发源
     * @return SnapshotResult：ok=true 时 path/size 有效，ok=false 时 errMsg 有原因
     */
    SnapshotResult snapshotNow(const std::string& prefix);

private:
    CameraService&    m_camera;

    mutable std::mutex m_cfgMutex;
    SnapshotCfg        m_cfg;

    /* 拍照执行锁：snapshotNow 全程持有，pauseForRestart 借它排空在途拍照 */
    std::mutex        m_execMutex;
    std::atomic<bool> m_paused{false};   ///< 相机重启窗口内拒绝拍照
};

} // namespace mediad
