/**
 * @file RecordService.cpp
 * @brief 录像服务实现
 */
#include "services/RecordService.h"
#include "services/CameraService.h"

#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace mediad {

RecordService::~RecordService() {
    stop();
}

void RecordService::setFileReadyCallback(FileReadyCallback cb) {
    m_onFileReady = std::move(cb);
    /* 转发 Mp4Recorder 的段完成通知（分段切换/收卷都会触发） */
    m_recorder.setFileCompleteCallback(
        [this](const std::string& path, size_t size, uint64_t durationMs,
               uint64_t startWallSec) {
            if (!m_onFileReady) {
                return;
            }
            /* 获取录像期间抓取的缩略图（已经直接保存到最终位置） */
            std::string thumbPath;
            int64_t thumbSize = 0;
            {
                std::lock_guard<std::mutex> lock(m_thumbMutex);
                thumbPath = m_currentThumbDestPath;
            }
            if (!thumbPath.empty()) {
                /* 获取缩略图文件大小 */
                FILE* fp = fopen(thumbPath.c_str(), "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    thumbSize = ftell(fp);
                    fclose(fp);
                }
                NC_LOGI("RecordService: video thumbnail saved to {} ({} bytes)",
                        thumbPath.c_str(), thumbSize);
            } else {
                NC_LOGW("RecordService: no video thumbnail available for {}", path.c_str());
            }
            m_onFileReady("record", path, static_cast<int64_t>(size), durationMs,
                          startWallSec, thumbPath, thumbSize);
        });
}

void RecordService::start(const RecordCfg& cfg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cfg = cfg;
    /* 相机此时通常还没就绪，实际起录等 onCameraStarted */
}

void RecordService::onCameraStarted() {
    /* 录像时机由 TriggerManager 控制，本服务不做主动起录 */
}

void RecordService::apply(const RecordCfg& cfg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const RecordCfg oldCfg = m_cfg;
    m_cfg = cfg;

    /* 配置变化时打印日志，方便排查 */
    if (oldCfg.segmentSec != cfg.segmentSec) {
        NC_LOGI("RecordService: segment_sec changed {} -> {}", oldCfg.segmentSec, cfg.segmentSec);
    }
    if (oldCfg.outputDir != cfg.outputDir) {
        NC_LOGI("RecordService: outputDir changed");
    }

    if (m_mode == Mode::Idle) {
        /* 空闲状态：配置已保存，下次起录时生效 */
        return;
    }

    /* 命令录像进行中：如果分段时长或目录变了，收卷当前文件、用新配置重新开始 */
    if (oldCfg.segmentSec != cfg.segmentSec || oldCfg.outputDir != cfg.outputDir) {
        NC_LOGI("RecordService: restarting recording with new config");
        stopRecordingLocked();
        /* 用新配置重新开始录像，时长沿用原来的 durationSec */
        std::string errMsg;
        if (!startRecordingLocked(m_mode, m_durationSec, errMsg)) {
            NC_LOGW("RecordService: restart failed: {}", errMsg.c_str());
            m_mode = Mode::Idle;
        }
    }
}

bool RecordService::startCommand(int durationSec, std::string& errMsg) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_mode != Mode::Idle) {
            errMsg = "recording in progress";
            return false;   /* 冲突拒绝回 Busy（用户裁决的简单稳定方案） */
        }
        if (!startRecordingLocked(Mode::Command, durationSec, errMsg)) {
            return false;
        }
    }
    NC_LOGI("RecordService: command recording started (duration={}s)", durationSec);
    return true;
}

bool RecordService::stopCommand(std::string& errMsg) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_mode != Mode::Command) {
            errMsg = "no command recording in progress";
            return false;
        }
        stopRecordingLocked();
        m_mode = Mode::Idle;
    }
    /* 锁外结束时长监控线程（线程内部会抢 m_mutex，持锁 join 会死锁） */
    m_durCancel = true;
    m_durCv.notify_all();
    if (m_durThread.joinable()) {
        m_durThread.join();
    }
    NC_LOGI("RecordService: command recording stopped");
    return true;
}

void RecordService::pauseForRestart() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_mode != Mode::Idle) {
            stopRecordingLocked();
            m_mode = Mode::Idle;
        }
    }
    /* 命令录像被分辨率变更打断：直接收卷结束，不跨重启续录（简单稳定） */
    if (m_durThread.joinable()) {
        m_durCancel = true;
        m_durCv.notify_all();
        m_durThread.join();
        NC_LOGW("RecordService: command recording finalized due to camera restart");
    }
    /* 缩略图后台线程也要收掉，免得重启期间还去抓帧 */
    if (m_thumbThread.joinable()) {
        m_thumbThread.join();
    }
}

void RecordService::resumeAfterRestart() {
    /* 录像时机由 TriggerManager 控制，相机重启后不主动恢复录像 */
}

void RecordService::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stopRecordingLocked();
        m_mode = Mode::Idle;
    }
    m_durCancel = true;
    m_durCv.notify_all();
    if (m_durThread.joinable()) {
        m_durThread.join();
    }
    if (m_thumbThread.joinable()) {
        m_thumbThread.join();
    }
}

RecordService::Mode RecordService::mode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mode;
}

std::string RecordService::currentFilePath() const {
    return m_recorder.currentFilePath();
}

bool RecordService::startRecordingLocked(Mode mode, int durationSec, std::string& errMsg) {
    if (!m_camera.isRunning()) {
        errMsg = "camera not running";
        return false;
    }

    /* 录像参数取自相机实际生效配置（而非 ConfigStore，防止热更中间态不一致） */
    const camera::ChannelConfig& mainChn = m_camera.device()->config().mainChannel();
    recorder::RecorderConfig rc;
    rc.width          = mainChn.width;
    rc.height         = mainChn.height;
    rc.fps            = mainChn.fps;
    rc.h265           = (mainChn.codec == camera::CodecType::H265);
    rc.segmentSeconds = static_cast<uint32_t>(m_cfg.segmentSec);
    rc.outputDir      = m_cfg.outputDir;

    if (!m_recorder.start(rc)) {
        errMsg = "mp4 recorder start failed";
        return false;
    }

    /* 主通道帧直灌录像器 */
    m_subId = m_camera.device()->subscribe(
        camera::ChannelId::Main, [this](const camera::VideoFrame& frame) {
            m_recorder.writeFrame(frame.data(), frame.size(),
                                  static_cast<uint64_t>(frame.timestamp()),
                                  frame.isKeyFrame());
            /* 分段切换发生在 writeFrame 内部（旧段已收卷、新文件已打开），
             * 这里检查文件是否换了，换了就把缩略图刷到新文件名下 */
            refreshThumbAfterRotate();
        });
    if (m_subId == camera::kInvalidSubscriptionId) {
        m_recorder.stop();
        errMsg = "subscribe main channel failed";
        return false;
    }

    m_mode = mode;

    /* 命令录像 + 有限时长：起监控线程到点自动收卷 */
    if (mode == Mode::Command && durationSec > 0) {
        if (m_durThread.joinable()) {
            m_durThread.join();   /* 回收上一次已结束的监控线程 */
        }
        m_durationSec = durationSec;
        m_durCancel = false;
        m_durThread = std::thread(&RecordService::durationLoop, this);
    }

    /* 启动缩略图守护：记录起始帧序号，启动移动侦测 */
    startThumbnailGuard();

    return true;
}

void RecordService::stopRecordingLocked() {
    /* 停止缩略图守护（移动侦测 + 兜底帧抓取） */
    stopThumbnailGuard();

    if (m_subId != camera::kInvalidSubscriptionId) {
        m_camera.device()->unsubscribe(m_subId);
        m_subId = camera::kInvalidSubscriptionId;
    }
    m_recorder.stop();   /* 收卷当前文件 */
}

void RecordService::durationLoop() {
    {
        std::unique_lock<std::mutex> lock(m_durMutex);
        m_durCv.wait_for(lock, std::chrono::seconds(m_durationSec),
                         [this] { return m_durCancel.load(); });
    }
    if (m_durCancel) {
        return;   /* 被 stopCommand/stop 主动结束 */
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_mode == Mode::Command) {
        stopRecordingLocked();
        m_mode = Mode::Idle;
        NC_LOGI("RecordService: command recording finished (duration reached)");
    }
}

/* ---------------------------------------------------------------
 * 缩略图守护：录像期间移动侦测抓变化帧，收卷时兜底
 * --------------------------------------------------------------- */

void RecordService::startThumbnailGuard() {
    /* 记录起始帧序号，等第 5 帧抓初始缩略图 */
    m_startFrameSeq = m_camera.device()->getThumbFrameSeq();
    m_initialFrameSeq = m_startFrameSeq + 4;  // 第 5 帧
    m_initialThumbCaptured = false;
    m_motionThumbCaptured = false;
    m_lastMotionThumbTime = std::chrono::steady_clock::time_point{};  // 重置节流时间戳

    /* 生成缩略图最终路径（和录像文件名对应），并记下守护对应的录像文件，
     * 之后分段切换时靠对比这两个路径判断要不要刷新 */
    std::string recordPath = m_recorder.currentFilePath();
    std::string thumbDestPath = nc::common::ThumbPathFromSource(recordPath);

    /* 清空当前缩略图路径 */
    {
        std::lock_guard<std::mutex> lock(m_thumbMutex);
        m_currentThumbPath.clear();
        m_currentThumbDestPath = thumbDestPath;
        m_guardRecordPath = recordPath;
    }

    /* 启动移动侦测，检测到运动时抓一张缩略图（一次录像只抓一张） */
    camera::MotionDetectConfig mdCfg;
    mdCfg.sensitivity = 80;
    mdCfg.confirmFrames = 1;
    mdCfg.intervalMs = 200;

    auto err = m_camera.device()->startMotionDetection(mdCfg,
        [this](bool motion, const camera::MotionRegion* /*regions*/, int /*count*/) {
            if (!motion) return;
            /* 已经抓过运动缩略图就不再抓，一次录像只需要一张 */
            if (m_motionThumbCaptured) return;
            this->captureMotionThumbnail();
        });

    if (err != camera::Error::Ok) {
        NC_LOGW("RecordService: motion detection start failed (err={}), "
                 "thumbnail will use fallback frame", static_cast<int>(err));
    }

    /* 启动后台线程等待第 5 帧并抓初始缩略图（线程存成员变量，stop 时能 join） */
    m_thumbThread = std::thread([this]() {
        /* 等待第 5 帧（约 0.5 秒）或超时 1 秒 */
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            uint64_t currentSeq = m_camera.device()->getThumbFrameSeq();
            if (currentSeq >= m_initialFrameSeq) {
                /* 抓到第 5 帧，存为初始缩略图 */
                this->saveThumbnailToFile("initial");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        /* 超时：用当前帧兜底 */
        this->saveThumbnailToFile("initial");
    });
}

void RecordService::stopThumbnailGuard() {
    /* 停止移动侦测 */
    m_camera.device()->stopMotionDetection();

    /* 清掉守护路径：录像停了之后帧回调里的刷新检查直接跳过 */
    {
        std::lock_guard<std::mutex> lock(m_thumbMutex);
        m_guardRecordPath.clear();
    }

    /* 等后台缩略图线程跑完再收场（线程里会抓帧、写文件，不能让它碰已释放的资源） */
    if (m_thumbThread.joinable()) {
        m_thumbThread.join();
    }

    /* 如果全程没抓到运动帧，抓收卷帧兜底 */
    if (!m_motionThumbCaptured) {
        saveThumbnailToFile("fallback");
    }
}

void RecordService::refreshThumbAfterRotate() {
    /* 录像器分段时会自己换文件，但缩略图路径是起录时定死的。
     * 每帧检查一下：录像文件变了就把缩略图也换到新文件名下，
     * 不这么做的话后面所有段都挂着第一段的名字和图片 */
    std::string recordPath = m_recorder.currentFilePath();
    if (recordPath.empty()) {
        return;   /* 录像已停止（没在写文件），不刷新 */
    }

    std::string thumbDestPath;
    {
        std::lock_guard<std::mutex> lock(m_thumbMutex);
        if (m_guardRecordPath.empty() || recordPath == m_guardRecordPath) {
            return;   /* 守护已停或还没换段，什么都不做 */
        }
        thumbDestPath = nc::common::ThumbPathFromSource(recordPath);
        m_guardRecordPath = recordPath;
        m_currentThumbPath.clear();
        m_currentThumbDestPath = thumbDestPath;
        /* 新段允许再抓一张运动缩略图 */
        m_motionThumbCaptured = false;
        m_lastMotionThumbTime = std::chrono::steady_clock::time_point{};
    }
    NC_LOGI("RecordService: segment rotated, thumbnail switches to {}",
            thumbDestPath.c_str());

    /* 抓当前帧作为新段的初始缩略图；captureThumbnail 要花几十毫秒，
     * 放后台线程做，免得堵住帧回调影响录像写盘（线程存成员变量，stop 时能 join） */
    m_thumbThread = std::thread([this]() { this->saveThumbnailToFile("segment"); });
}

void RecordService::captureMotionThumbnail() {
    /* 节流：两次抓取之间至少间隔 10 秒，避免频繁写盘损伤 flash */
    auto now = std::chrono::steady_clock::now();
    if (m_lastMotionThumbTime.time_since_epoch().count() != 0 &&
        now - m_lastMotionThumbTime < std::chrono::seconds(10)) {
        return;
    }
    m_lastMotionThumbTime = now;

    std::vector<uint8_t> thumbData;
    if (m_camera.device()->captureThumbnail(thumbData) != camera::Error::Ok ||
        thumbData.empty()) {
        return;
    }
    /* 标记已抓过运动缩略图：stopThumbnailGuard 靠这个标志决定要不要补 fallback */
    m_motionThumbCaptured = true;
    saveThumbnailToFile("motion");
}

/* 抓一帧缩略图并写到当前最终路径（和录像文件名对应）。
 * label 只用在日志里，区分是哪种场景抓的（initial/motion/fallback/segment）。
 * 三个 capture 函数的公共部分：抓帧 → 读目标路径 → 写文件 → 记日志 */
void RecordService::saveThumbnailToFile(const char* label) {
    std::vector<uint8_t> thumbData;
    if (m_camera.device()->captureThumbnail(thumbData) != camera::Error::Ok ||
        thumbData.empty()) {
        NC_LOGW("RecordService: failed to capture {} thumbnail", label);
        return;
    }

    std::string thumbPath;
    {
        std::lock_guard<std::mutex> lock(m_thumbMutex);
        thumbPath = m_currentThumbDestPath;
    }
    if (thumbPath.empty()) {
        NC_LOGW("RecordService: no thumbnail destination path set for {}", label);
        return;
    }

    FILE* fp = fopen(thumbPath.c_str(), "wb");
    if (fp) {
        fwrite(thumbData.data(), 1, thumbData.size(), fp);
        fclose(fp);
        NC_LOGI("RecordService: {} thumbnail saved {} ({} bytes)",
                label, thumbPath.c_str(), thumbData.size());
    } else {
        NC_LOGW("RecordService: failed to save {} thumbnail: {}", label, thumbPath.c_str());
    }
}

} // namespace mediad
