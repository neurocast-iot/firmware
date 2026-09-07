/**
 * @file mp4_demuxer.cpp
 * @brief MP4 解封装读帧器实现（平台驱动封装）
 *
 * 业务逻辑（裁剪时段、帧过滤）留在本文件，
 * SDK 调用全部下沉到 DemuxerDriver 平台实现（如 AnykaDemuxerDriver）。
 */
#include "recorder/mp4_demuxer.h"
#include "driver_factory.h"

#include "nc/common/log_utils.h"

namespace recorder {

Mp4Demuxer::Mp4Demuxer()
    : m_driver(createDemuxerDriver()) {
}

Mp4Demuxer::~Mp4Demuxer() {
    close();
}

bool Mp4Demuxer::open(const std::string& filePath, std::string& errMsg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_opened) {
        errMsg = "demuxer already opened: " + m_path;
        return false;
    }

    /* 调驱动打开文件并解析视频流信息 */
    uint64_t totalMs = 0;
    DemuxVideoInfo videoInfo;
    if (!m_driver->open(filePath, m_handle, totalMs, videoInfo)) {
        errMsg = "driver open failed: " + filePath;
        return false;
    }

    m_totalMs   = totalMs;
    m_videoInfo = videoInfo;
    m_path      = filePath;
    m_opened    = true;
    NC_LOGI("[demux] 打开成功: file={} 时长={}ms {}x{}@{}fps {}",
            filePath, m_totalMs, m_videoInfo.width, m_videoInfo.height,
            m_videoInfo.fps, m_videoInfo.h265 ? "H265" : "H264");
    return true;
}

void Mp4Demuxer::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    /* 调驱动关闭 demux（内部处理文件句柄） */
    if (m_handle >= 0) {
        m_driver->close(m_handle);
        m_handle = -1;
    }
    m_opened = false;
    m_totalMs = 0;
    m_videoInfo = DemuxVideoInfo{};
    m_path.clear();
}

bool Mp4Demuxer::isOpen() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_opened;
}

uint64_t Mp4Demuxer::totalDurationMs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalMs;
}

DemuxVideoInfo Mp4Demuxer::videoInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_videoInfo;
}

std::string Mp4Demuxer::filePath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}

int64_t Mp4Demuxer::seekToKeyframe(uint64_t offsetMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened) {
        return -1;
    }
    /* 调驱动跳到关键帧 */
    return m_driver->seekToKeyframe(m_handle, offsetMs);
}

bool Mp4Demuxer::readNextFrame(DemuxFrame& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened) {
        return false;
    }
    /* 调驱动读下一帧（内部处理 SDK 头剥离、音频帧跳过等） */
    return m_driver->readNextFrame(m_handle, frame);
}

} // namespace recorder
