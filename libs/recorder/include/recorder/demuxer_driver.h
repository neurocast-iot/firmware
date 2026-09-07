/**
 * @file demuxer_driver.h
 * @brief 解封装驱动接口（内部使用，非公共 API）
 *
 * 封装平台特定的 MP4 解封装操作（Anyka 的 ak_demux_* 或等价 API）。
 * 公共层（Mp4Demuxer）通过此接口与平台交互，自己不直接调用 SDK。
 *
 * 设计原则：
 *   - 接口尽量贴近 SDK 原始调用序列，降低实现复杂度
 *   - 状态管理（demux 句柄、文件句柄）由驱动实现负责
 *   - 公共层只管业务逻辑（裁剪时段、帧过滤）
 *
 * 新增平台 = 新写一个 src/<平台>/anyka_demuxer_driver.cpp 实现此接口，
 * 公共层（mp4_demuxer.cpp）零改动。
 */
#pragma once

#include "recorder/mp4_demuxer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace recorder {

/**
 * @brief 解封装驱动纯虚接口
 *
 * 封装 MP4 解封装的完整生命周期：open → getVideoInfo → seekToKeyframe → readNextFrame(循环) → close。
 * 平台实现负责管理 SDK 句柄、文件句柄、帧内存等底层细节。
 */
class DemuxerDriver {
public:
    virtual ~DemuxerDriver() = default;

    /**
     * 打开 MP4 文件并解析视频流信息
     *
     * 内部完成：fopen → ak_demux_open → ak_demux_get_total_time → ak_demux_get_video_info。
     * 失败时自动关闭已打开的文件句柄，调用方无需额外处理。
     *
     * @param filePath MP4 文件完整路径
     * @param[out] handle 解封装句柄（成功时 >=0）
     * @param[out] totalMs 文件总时长（毫秒），拿不到时为 0
     * @param[out] videoInfo 视频流信息（宽高/帧率/编码格式）
     * @return true=打开成功，false=文件打不开 / 不是合法 MP4 / 无视频流
     */
    virtual bool open(const std::string& filePath, int& handle, uint64_t& totalMs,
                      DemuxVideoInfo& videoInfo) = 0;

    /**
     * 关闭解封装并释放资源
     *
     * 内部完成：ak_demux_close → fclose。
     * 关闭顺序必须先 demux 后文件（SDK 内部还在用 FILE*）。
     *
     * @param handle 解封装句柄
     */
    virtual void close(int handle) = 0;

    /**
     * 按时间偏移跳到关键帧
     *
     * 找的是"不晚于 offsetMs 的最近关键帧"：裁剪起点往前对齐到 I 帧，
     * 保证用户要的时段一帧不少（代价是最多多带一个 GOP 的开头）。
     *
     * @param handle 解封装句柄
     * @param offsetMs 距文件开头的毫秒偏移
     * @return 实际落点关键帧的时间戳（毫秒），-1=未打开或定位失败
     */
    virtual int64_t seekToKeyframe(int handle, uint64_t offsetMs) = 0;

    /**
     * 顺序读下一帧视频数据
     *
     * 内部完成：ak_demux_get_data → 剥掉 SDK 前置的 8 字节"序列号+时间戳"头 →
     * 拷贝纯编码数据到 frame.data → ak_demux_free_data。
     * 自动跳过音频帧和空帧（len <= 8）。
     *
     * @param handle 解封装句柄
     * @param[out] frame 输出帧（数据拷贝进 frame.data，SDK 内存内部释放）
     * @return true=读到一帧，false=读到文件尾或失败
     */
    virtual bool readNextFrame(int handle, DemuxFrame& frame) = 0;
};

} // namespace recorder
