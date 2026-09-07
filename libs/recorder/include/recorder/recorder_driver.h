/**
 * @file recorder_driver.h
 * @brief 录像驱动接口（内部使用，非公共 API）
 *
 * 封装平台特定的 MP4 封装操作（Anyka 的 ak_mux_* 或等价 API）。
 * 公共层（Mp4Recorder）通过此接口与平台交互，自己不直接调用 SDK。
 *
 * 设计原则：
 *   - 接口尽量贴近 SDK 原始调用序列，降低实现复杂度
 *   - 状态管理（mux 句柄、缓冲区）由驱动实现负责
 *   - 公共层只管业务逻辑（分段、回调、文件命名）
 *
 * 新增平台 = 新写一个 src/<平台>/anyka_recorder_driver.cpp 实现此接口，
 * 公共层（mp4_recorder.cpp）零改动。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recorder {

/**
 * @brief 录像驱动纯虚接口
 *
 * 封装 MP4 封装容器的完整生命周期：open → start → addVideo(循环) → stop → close → fixFile。
 * 平台实现负责管理 SDK 句柄、缓冲区、临时文件等底层细节。
 */
class RecorderDriver {
public:
    virtual ~RecorderDriver() = default;

    /**
     * 打开 mux 容器并启动
     *
     * 内部完成：分配缓冲区 → ak_mux_open → ak_mux_start → 准备接收帧。
     * 失败时自动清理已分配资源，调用方无需额外处理。
     *
     * @param width 视频宽度（必须与喂入帧的实际编码参数一致）
     * @param height 视频高度
     * @param fps 帧率
     * @param h265 true=H.265，false=H.264
     * @param enableAudio 是否启用音频轨（当前恒为 false，预留）
     * @param filePath 输出文件完整路径
     * @param segmentSeconds 分段时长（秒），0=不分段
     * @return 成功返回 mux 句柄（>=0），失败返回 -1
     */
    virtual int openAndStart(uint16_t width, uint16_t height, uint8_t fps, bool h265,
                             bool enableAudio, const std::string& filePath,
                             uint32_t segmentSeconds) = 0;

    /**
     * 写入一帧编码数据
     *
     * 内部构造 SDK 帧结构并调用 ak_mux_add_video。
     * 时间戳回退、非关键帧丢弃等逻辑由公共层处理，驱动只管写入。
     *
     * @param handle mux 句柄（openAndStart 返回值）
     * @param data 编码数据（H264/H265 NALU，含起始码）
     * @param len 数据长度
     * @param timestamp 帧时间戳（毫秒）
     * @param isKeyFrame 是否关键帧
     * @return true=写入成功，false=写入失败
     */
    virtual bool addVideo(int handle, const uint8_t* data, size_t len,
                          uint64_t timestamp, bool isKeyFrame) = 0;

    /**
     * 停止 mux 并关闭容器
     *
     * 内部完成：ak_mux_stop → ak_mux_close → 释放缓冲区。
     * 不触发文件修复（fixFile 单独调用，因为可能批量处理多个文件）。
     *
     * @param handle mux 句柄
     */
    virtual void stopAndClose(int handle) = 0;

    /**
     * 修复 mux 生成的临时文件，合并为完整 MP4
     *
     * 内部完成：ak_mux_fix_file → 删除 .idx/.moov/.stbl 临时文件。
     * 临时文件可能不存在（SDK 已自动合成），此时返回 false 属正常。
     *
     * @param filePath MP4 文件完整路径
     * @param[out] durationMs 文件时长（毫秒），fix 接口不给时用 0 填充
     * @return true=修复成功，false=修复失败或无需修复
     */
    virtual bool fixFile(const std::string& filePath, uint64_t& durationMs) = 0;
};

} // namespace recorder
