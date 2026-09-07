/**
 * @file SnapshotService.cpp
 * @brief 拍照服务实现（纯执行器，定时由 TriggerManager 驱动）
 */
#include "services/SnapshotService.h"
#include "services/CameraService.h"

#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"

#include <chrono>
#include <ctime>

namespace mediad {

namespace {

/* 生成 outputDir/yyyyMMdd/HH/{prefix}yyyyMMdd_HHMMSS{ms}.jpg 路径（含建目录）
 * prefix 区分触发源（如 "timer_"、"bluetooth_"）；
 * 毫秒精度防止连拍同一秒内文件名冲突互相覆盖 */
std::string buildSnapshotPath(const std::string& outputDir, const std::string& prefix) {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    int ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;

    std::tm tmBuf;
    localtime_r(&tt, &tmBuf);

    char dayDir[16];
    char hourDir[8];
    char fileName[48];
    std::strftime(dayDir, sizeof(dayDir), "%Y%m%d", &tmBuf);
    std::strftime(hourDir, sizeof(hourDir), "%H", &tmBuf);
    std::strftime(fileName, sizeof(fileName), "%Y%m%d_%H%M%S", &tmBuf);

    /* 拼接：前缀 + 时间 + 毫秒 */
    std::string name = prefix + fileName + std::to_string(ms) + ".jpg";

    std::string dir = outputDir + "/" + dayDir + "/" + hourDir;
    if (!nc::common::MakeDirs(dir)) {
        return "";
    }
    return dir + "/" + name;
}

} // namespace

void SnapshotService::start(const SnapshotCfg& cfg) {
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        m_cfg = cfg;
    }
    NC_LOGI("SnapshotService: started (outputDir={}, quality={})",
            cfg.outputDir.c_str(), cfg.quality);
}

void SnapshotService::apply(const SnapshotCfg& cfg) {
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        m_cfg = cfg;
    }
    NC_LOGI("SnapshotService: config applied (outputDir={}, quality={})",
            cfg.outputDir.c_str(), cfg.quality);
}

void SnapshotService::stop() {
    /* 纯执行器，无资源需释放 */
    NC_LOGI("SnapshotService: stopped");
}

void SnapshotService::pauseForRestart() {
    m_paused.store(true);
    /* 拿一次执行锁：在途拍照拍完才能拿到，拿到即排空完成；
     * 此后新请求被 m_paused 拦截，相机重启可安全进行 */
    { std::lock_guard<std::mutex> lock(m_execMutex); }
    NC_LOGI("SnapshotService: paused for camera restart");
}

void SnapshotService::resumeAfterRestart() {
    if (m_paused.exchange(false)) {
        NC_LOGI("SnapshotService: resumed after camera restart");
    }
}

SnapshotResult SnapshotService::snapshotNow(const std::string& prefix) {
    SnapshotResult result;

    /* 执行锁全程持有：串行多个触发源，也让 pauseForRestart
     * 能等到在途拍照结束；持锁后再查暂停标志，排除“检查时未暂停、
     * 拿锁时已进入重启窗口”的竞态 */
    std::lock_guard<std::mutex> execLock(m_execMutex);
    if (m_paused.load()) {
        result.errMsg = "camera restarting";
        return result;
    }

    /* 前置状态检查：相机未跑直接拒绝，不碰 SDK */
    if (!m_camera.isRunning()) {
        result.errMsg = "camera not running";
        return result;
    }

    SnapshotCfg cfg;
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        cfg = m_cfg;
    }

    result.path = buildSnapshotPath(cfg.outputDir, prefix);
    if (result.path.empty()) {
        result.errMsg = "create output dir failed: " + cfg.outputDir;
        return result;
    }

    /* 快照走子通道：与主通道录像/推流隔离；通道已由服务启动时保活，
     * 此处直接取帧编码 */
    camera::Error err = m_camera.device()->captureSnapshot(
        camera::ChannelId::Sub, result.path, cfg.quality);
    if (err != camera::Error::Ok) {
        result.errMsg = std::string("captureSnapshot failed: ") + camera::errorToString(err);
        NC_LOGE("SnapshotService: {}", result.errMsg.c_str());
        return result;
    }

    result.size = nc::common::FileSize(result.path);

    /* 拍照成功后，同时抓取缩略图（从硬件 VENC 缩略图编码器 buffer 读）
     * 缩略图用于上传到服务器替代原图，节省流量 */
    std::vector<uint8_t> thumbData;
    err = m_camera.device()->captureThumbnail(thumbData);
    if (err == camera::Error::Ok && !thumbData.empty()) {
        /* 缩略图文件名：原图路径去掉扩展名 + _thumb.jpg（和录像共用同一套算法） */
        std::string thumbPath = nc::common::ThumbPathFromSource(result.path);

        FILE* fp = fopen(thumbPath.c_str(), "wb");
        if (fp) {
            fwrite(thumbData.data(), 1, thumbData.size(), fp);
            fclose(fp);
            result.thumbPath = thumbPath;
            result.thumbSize = static_cast<int64_t>(thumbData.size());
            NC_LOGD("SnapshotService: thumbnail saved {} ({} bytes)",
                    thumbPath.c_str(), result.thumbSize);
        } else {
            NC_LOGW("SnapshotService: failed to save thumbnail: {}", thumbPath.c_str());
        }
    } else {
        /* 缩略图拿不到不阻塞拍照：可能缩略图编码器还没启动，降级运行 */
        NC_LOGW("SnapshotService: no thumbnail available (err={})",
                camera::errorToString(err));
    }

    result.ok = true;
    NC_LOGD("SnapshotService: snapshot saved {} ({} bytes)",
            result.path.c_str(), result.size);
    return result;
}

} // namespace mediad
