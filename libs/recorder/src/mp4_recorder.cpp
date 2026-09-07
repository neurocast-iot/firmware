/**
 * @file mp4_recorder.cpp
 * @brief MP4 录像器实现（平台驱动封装）
 *
 * 业务逻辑（分段、回调、文件命名）留在本文件，
 * SDK 调用全部下沉到 RecorderDriver 平台实现（如 AnykaRecorderDriver）。
 *
 * 配方来源：gw_av100 iot_live-cpp VideoRecorderManager 取证复刻。
 */
#include "recorder/mp4_recorder.h"
#include "driver_factory.h"

#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>

namespace recorder {

Mp4Recorder::Mp4Recorder()
    : m_driver(createRecorderDriver()) {
}

Mp4Recorder::~Mp4Recorder() {
    stop();
}

void Mp4Recorder::setFileCompleteCallback(FileCompleteCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onFileComplete = std::move(cb);
}

bool Mp4Recorder::start(const RecorderConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_recording) {
        NC_LOGW("Mp4Recorder: already recording ({})", m_currentPath.c_str());
        return false;
    }
    if (config.outputDir.empty()) {
        NC_LOGE("Mp4Recorder: outputDir is empty");
        return false;
    }

    m_config = config;
    if (!openMux()) {
        return false;
    }

    m_recording = true;
    NC_LOGI("Mp4Recorder: recording started: {} (segment={}s)",
            m_currentPath.c_str(), m_config.segmentSeconds);
    return true;
}

bool Mp4Recorder::writeFrame(const uint8_t* data, size_t len,
                             uint64_t timestamp, bool isKeyFrame) {
    std::lock_guard<std::mutex> lock(m_mutex);

    /* 状态与参数前置检查（含分段切换竞态窗口的句柄检查） */
    if (!m_recording || m_muxHandle == -1) {
        return false;
    }
    if (!data || len == 0) {
        return false;
    }

    /* 时间戳回退帧丢弃，防止 elapsed 下溢（原配方） */
    if (!m_waitingKeyFrame && timestamp < m_segmentStartTs) {
        NC_LOGW("Mp4Recorder: timestamp rollback {} < {}, drop frame",
                timestamp, m_segmentStartTs);
        return false;
    }

    /* MP4 必须以 I 帧开始，之前的 P 帧全部丢弃 */
    if (m_waitingKeyFrame) {
        if (!isKeyFrame) {
            return false;
        }
        m_waitingKeyFrame = false;
        m_frameSeq        = 0;
        m_segmentStartTs  = timestamp;
        /* 记下这段录像第一帧写入的墙上时间，收卷时随回调上报（start_time） */
        m_segmentStartWallSec = static_cast<uint64_t>(std::time(nullptr));
        NC_LOGI("Mp4Recorder: key frame received, writing begins (ts={})",
                timestamp);
    }

    /* 关键帧处检查分段时长，到点收卷当前文件并开新段 */
    if (isKeyFrame && m_config.segmentSeconds > 0) {
        uint64_t elapsedMs   = timestamp - m_segmentStartTs;
        uint64_t thresholdMs = (uint64_t)m_config.segmentSeconds * 1000;
        if (elapsedMs >= thresholdMs) {
            NC_LOGI("Mp4Recorder: segment reached {}ms (config={}s), rotating",
                    elapsedMs, m_config.segmentSeconds);
            closeMux(true);
            if (!openMux()) {
                NC_LOGE("Mp4Recorder: reopen mux failed after segment, recording stopped");
                m_recording = false;
                return false;
            }
            /* 新段以当前关键帧起步：直接落入下方写入路径 */
            m_waitingKeyFrame = false;
            m_frameSeq        = 0;
            m_segmentStartTs  = timestamp;
            /* 新段首帧的墙上时间同样记下（旧段的值已在 closeMux 里随回调报出） */
            m_segmentStartWallSec = static_cast<uint64_t>(std::time(nullptr));
        }
    }

    /* 调驱动写入帧数据 */
    if (!m_driver->addVideo(m_muxHandle, data, len, timestamp, isKeyFrame)) {
        NC_LOGW("Mp4Recorder: driver addVideo failed len={} kf={}", len, isKeyFrame);
        return false;
    }
    /* 记下最后一帧时间戳：收卷时 fix 接口不给时长就用 首末帧时间差 自己算 */
    m_lastFrameTs = timestamp;
    return true;
}

void Mp4Recorder::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording) {
        return;
    }
    m_recording = false;
    closeMux(true);
    NC_LOGI("Mp4Recorder: recording stopped");
}

bool Mp4Recorder::reopenMux(uint32_t newSegmentSec) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || m_muxHandle == -1) {
        NC_LOGW("Mp4Recorder: reopenMux skipped (not recording)");
        return false;
    }

    /* 更新分段时长并重建 mux（与 writeFrame 内部分段切换同一路径） */
    m_config.segmentSeconds = newSegmentSec;
    closeMux(true);
    if (!openMux()) {
        NC_LOGE("Mp4Recorder: reopenMux failed, recording stopped");
        m_recording = false;
        return false;
    }
    NC_LOGI("Mp4Recorder: mux reopened with segment={}s", newSegmentSec);
    return true;
}

bool Mp4Recorder::isRecording() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_recording;
}

std::string Mp4Recorder::currentFilePath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_recording ? m_currentPath : std::string();
}

bool Mp4Recorder::openMux() {
    /* 生成文件路径：outputDir/yyyyMMdd/HH/yyyyMMdd_HHMMSS.mp4（原配方目录结构） */
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);

    char subDir[32];
    snprintf(subDir, sizeof(subDir), "%04d%02d%02d/%02d",
             tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday, tmNow.tm_hour);

    std::string dir = m_config.outputDir;
    if (!dir.empty() && dir.back() != '/') {
        dir += '/';
    }
    dir += subDir;
    if (!nc::common::MakeDirs(dir)) {
        NC_LOGE("Mp4Recorder: create dir failed: {}", dir.c_str());
        return false;
    }

    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &tmNow);
    m_currentPath = dir + "/" + timeBuf + ".mp4";

    /* 调驱动打开 mux 并启动 */
    m_muxHandle = m_driver->openAndStart(
        m_config.width, m_config.height, m_config.fps, m_config.h265,
        m_config.enableAudio, m_currentPath, m_config.segmentSeconds);
    if (m_muxHandle == -1) {
        NC_LOGE("Mp4Recorder: driver openAndStart failed");
        return false;
    }

    /* 新文件从关键帧起写 */
    m_waitingKeyFrame = true;
    m_frameSeq        = 0;
    m_segmentStartTs  = 0;
    m_lastFrameTs     = 0;
    m_segmentStartWallSec = 0;   /* 等首个关键帧真正写入时再记，这里先清零 */
    return true;
}

void Mp4Recorder::closeMux(bool finalizeFile) {
    if (m_muxHandle == -1) {
        return;
    }
    /* 调驱动停止并关闭 mux */
    m_driver->stopAndClose(m_muxHandle);
    m_muxHandle = -1;

    if (finalizeFile) {
        fixAndNotify(m_currentPath);
    }
}

void Mp4Recorder::fixAndNotify(const std::string& filePath) {
    /* 调驱动修复临时文件并获取时长 */
    uint64_t durationMs = 0;
    m_driver->fixFile(filePath, durationMs);

    /* 时长优先用 fix 接口返回值；SDK 自动收卷时它不给（total_ms=0），
     * 就用写入帧的首末时间戳自己算，帧都在手上这个值必然有 */
    if (durationMs == 0 && m_lastFrameTs > m_segmentStartTs) {
        durationMs = m_lastFrameTs - m_segmentStartTs;
        NC_LOGI("Mp4Recorder: duration from frame timestamps: {}ms", durationMs);
    }

    /* 取文件大小并触发完成回调 */
    struct stat st;
    size_t fileSize = (stat(filePath.c_str(), &st) == 0) ? (size_t)st.st_size : 0;
    NC_LOGI("Mp4Recorder: file complete: {} ({} bytes)", filePath.c_str(), fileSize);

    if (m_onFileComplete) {
        m_onFileComplete(filePath, fileSize, durationMs, m_segmentStartWallSec);
    }

    /* 分段文件完成后强制刷盘，把内存里的脏页写到 SD 卡，防止 page cache 堆积导致 OOM */
    sync();
}

} // namespace recorder
