/**
 * @file mp4_demuxer.h
 * @brief MP4 解封装读帧器（平台驱动封装）
 *
 * 职责边界：只管"打开一个已录好的 MP4，按时间定位、逐帧读出编码数据"——
 *   打开文件 → 查时长/视频信息 → 按毫秒跳到关键帧 → 顺序读帧。
 * 读出来的帧拿去做什么（裁剪重封装 / 回放解码）由调用方决定，本类不管。
 *
 * 与 Mp4Recorder 是一对：recorder 用驱动把帧写成 MP4，
 * demuxer 用驱动把 MP4 读回帧，两者配合可做"按时段裁剪录像"：
 *   seek 到起点前的关键帧 → 逐帧读到终点 → 喂给 Mp4Recorder 写成小文件。
 *
 * 多平台架构：
 *   - 公共层（本类）只管业务逻辑（裁剪时段、帧过滤）
 *   - SDK 调用全部下沉到 DemuxerDriver 平台实现（如 AnykaDemuxerDriver）
 *   - 新增平台 = 新写 src/<平台>/driver_factory.cpp，本类零改动
 *
 * 线程安全：所有公开方法内部加锁。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace recorder {

class DemuxerDriver;  // 前置声明，平台驱动接口

/** 文件里的视频流信息（open 成功后可读） */
struct DemuxVideoInfo {
    uint16_t width  = 0;    ///< 视频宽度
    uint16_t height = 0;    ///< 视频高度
    uint32_t fps    = 0;    ///< 帧率
    bool     h265   = false;///< true=H.265，false=H.264
};

/** 读出的一帧视频数据（数据归调用方所有，无需手动释放） */
struct DemuxFrame {
    std::vector<uint8_t> data;  ///< 纯编码数据（H264/H265 NALU，含起始码，SDK 头已去掉）
    uint64_t ts = 0;            ///< 帧在文件内的时间戳（毫秒，从文件开头算）
    bool isKeyFrame = false;    ///< 是否关键帧（I 帧）
};

class Mp4Demuxer {
public:
    Mp4Demuxer();
    ~Mp4Demuxer();

    /* 句柄型资源，禁止拷贝 */
    Mp4Demuxer(const Mp4Demuxer&) = delete;
    Mp4Demuxer& operator=(const Mp4Demuxer&) = delete;

    /**
     * @brief 打开 MP4 文件并解析流信息
     *
     * @param filePath MP4 文件完整路径
     * @param errMsg   失败原因（成功时不动）
     * @return true=打开成功；false=文件打不开 / 不是合法 MP4 / 无视频流
     */
    bool open(const std::string& filePath, std::string& errMsg);

    /** 关闭文件释放句柄（幂等，析构自动调用） */
    void close();

    bool isOpen() const;

    /** 文件总时长（毫秒），未打开返回 0 */
    uint64_t totalDurationMs() const;

    /** 视频流信息，未打开返回全 0 */
    DemuxVideoInfo videoInfo() const;

    /** 当前打开的文件路径（未打开为空） */
    std::string filePath() const;

    /**
     * @brief 按时间偏移跳到关键帧（裁剪定位用）
     *
     * 找的是"不晚于 offsetMs 的最近关键帧"：裁剪起点往前对齐到 I 帧，
     * 保证用户要的时段一帧不少（代价是最多多带一个 GOP 的开头）。
     *
     * @param offsetMs 距文件开头的毫秒偏移
     * @return 实际落点关键帧的时间戳（毫秒）；-1=未打开或定位失败
     */
    int64_t seekToKeyframe(uint64_t offsetMs);

    /**
     * @brief 顺序读下一帧视频（自动跳过音频帧和空帧）
     *
     * @param frame 输出帧（数据拷贝进 frame.data，SDK 内存内部释放）
     * @return true=读到一帧；false=读到文件尾或未打开
     */
    bool readNextFrame(DemuxFrame& frame);

private:
    mutable std::mutex m_mutex;

    std::unique_ptr<DemuxerDriver> m_driver;  ///< 平台驱动实例（构造时由工厂创建）
    int   m_handle = -1;        ///< 驱动返回的 demux 句柄（-1=无效）
    bool  m_opened = false;

    uint64_t       m_totalMs = 0;
    DemuxVideoInfo m_videoInfo;
    std::string    m_path;
};

} // namespace recorder
