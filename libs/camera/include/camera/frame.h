/**
 * @file frame.h
 * @brief 视频帧封装类
 *
 * VideoFrame 是库对外暴露的视频帧对象。
 * 生命周期完全由库内部管理，用户只在回调中读取数据，
 * 不需要也不允许手动 delete 或 free。
 *
 * 关键设计：
 *   - 不可拷贝（防止意外的引用计数混乱）
 *   - 可移动（支持高效传递）
 *   - 析构时通过注入的释放函数归还底层驱动资源
 *   - ChannelId 标识帧来源通道，便于多通道分发
 */
#pragma once

#include "types.h"
#include <cstdint>
#include <functional>
#include <memory>

namespace camera {

/* 前置声明：帧构造入口，所有平台驱动通过它构造 VideoFrame */
struct FrameFactory;

/**
 * @brief 视频帧
 *
 * 用法示例：
 * @code
 * camera.subscribe(ChannelId::Main, [](const VideoFrame& frame) {
 *     // 直接访问，无需 release
 *     process(frame.data(), frame.size());
 *
 *     if (frame.frameType() == FrameType::I) {
 *         markKeyFrame();
 *     }
 * });
 * @endcode
 */
class VideoFrame {
public:
    // --------------------------------------------------------
    // 禁止拷贝，允许移动
    // --------------------------------------------------------
    VideoFrame(const VideoFrame&)            = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&)                 = default;
    VideoFrame& operator=(VideoFrame&&)      = default;

    /**
     * @brief 析构函数
     *
     * 通过注入的 release 函数释放底层 SDK 资源。
     * 若 release 为 nullptr，则不执行任何释放操作。
     */
    ~VideoFrame();

    // --------------------------------------------------------
    // 数据访问
    // --------------------------------------------------------

    /**
     * @brief 原始编码数据指针（H264/H265 NALU 序列，含起始码）
     * @note 指针有效期仅限回调执行期间，不要持久化保存。
     */
    const uint8_t* data() const noexcept { return m_data; }

    /**
     * @brief 编码数据字节长度
     */
    size_t size() const noexcept { return m_size; }

    /**
     * @brief 帧时间戳（毫秒，来自平台的视频输入模块）
     */
    int64_t timestamp() const noexcept { return m_timestamp; }

    /**
     * @brief 帧序列号（单调递增，从0开始，可用于检测丢帧）
     */
    uint32_t sequence() const noexcept { return m_sequence; }

    /**
     * @brief 帧类型（I帧 / P帧 / PI帧）
     */
    FrameType frameType() const noexcept { return m_frameType; }

    /**
     * @brief 编码格式
     */
    CodecType codec() const noexcept { return m_codec; }

    /**
     * @brief 来源通道（主通道 / 子通道）
     */
    ChannelId channel() const noexcept { return m_channel; }

    /**
     * @brief 是否为关键帧（I帧）
     */
    bool isKeyFrame() const noexcept {
        return m_frameType == FrameType::I || m_frameType == FrameType::PI;
    }

    /**
     * @brief 视频宽度（像素）
     */
    uint16_t width() const noexcept { return m_width; }

    /**
     * @brief 视频高度（像素）
     */
    uint16_t height() const noexcept { return m_height; }

private:
    // --------------------------------------------------------
    // 只能通过 FrameFactory 构造（平台驱动统一入口）
    // --------------------------------------------------------
    friend struct FrameFactory;

    /**
     * @brief 私有构造函数
     *
     * @param data       编码数据指针（SDK 内部管理，不拷贝）
     * @param size       数据长度
     * @param timestamp  时间戳（ms）
     * @param sequence   帧序号
     * @param frameType  帧类型
     * @param codec      编码格式
     * @param channel    来源通道
     * @param width      视频宽度
     * @param height     视频高度
     * @param release    资源释放回调（析构时调用）
     */
    VideoFrame(const uint8_t* data,
               size_t         size,
               int64_t        timestamp,
               uint32_t       sequence,
               FrameType      frameType,
               CodecType      codec,
               ChannelId      channel,
               uint16_t       width,
               uint16_t       height,
               std::function<void()> release);

    // --------------------------------------------------------
    // 成员变量
    // --------------------------------------------------------
    const uint8_t* m_data      = nullptr;
    size_t         m_size      = 0;
    int64_t        m_timestamp = 0;
    uint32_t       m_sequence  = 0;
    FrameType      m_frameType = FrameType::P;
    CodecType      m_codec     = CodecType::H264;
    ChannelId      m_channel   = ChannelId::Main;
    uint16_t       m_width     = 0;
    uint16_t       m_height    = 0;

    /**
     * @brief 底层驱动资源释放函数
     *
     * 由构造方注入，析构时调用，负责归还平台的码流缓冲区
     * （如 Anyka 的 ak_venc_release_stream）。
     * 为 nullptr 时析构不做任何释放。
     */
    std::function<void()> m_release;
};

/**
 * @brief VideoFrame 构造入口（平台驱动内部使用）
 *
 * VideoFrame 的构造函数是私有的，各平台驱动都通过这里构造，
 * 新增平台不需要改 frame.h 加 friend。
 */
struct FrameFactory {
    /**
     * @brief 构造一帧（参数含义同 VideoFrame 私有构造函数）
     *
     * @param data    编码数据指针（驱动内部管理，不拷贝）
     * @param release 资源释放回调（析构时调用，可为空）
     */
    static VideoFrame make(const uint8_t* data,
                           size_t         size,
                           int64_t        timestamp,
                           uint32_t       sequence,
                           FrameType      frameType,
                           CodecType      codec,
                           ChannelId      channel,
                           uint16_t       width,
                           uint16_t       height,
                           std::function<void()> release);
};

} // namespace camera
