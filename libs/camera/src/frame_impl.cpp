/**
 * @file frame_impl.cpp
 * @brief VideoFrame 实现
 *
 * 包含 VideoFrame 的构造函数、析构函数和 FrameFactory 实现。
 * 析构函数通过注入的释放函数归还平台码流缓冲区。
 */
#include "camera/frame.h"
#include <functional>
#include <utility>

namespace camera {

/**
 * @brief 构造函数（私有，仅 FrameFactory 可调用）
 *
 * data 指针由驱动内部管理，VideoFrame 不拷贝数据，
 * 仅持有指针和释放函数。
 */
VideoFrame::VideoFrame(const uint8_t*        data,
                       size_t                size,
                       int64_t               timestamp,
                       uint32_t              sequence,
                       FrameType             frameType,
                       CodecType             codec,
                       ChannelId             channel,
                       uint16_t              width,
                       uint16_t              height,
                       std::function<void()> release)
    : m_data(data)
    , m_size(size)
    , m_timestamp(timestamp)
    , m_sequence(sequence)
    , m_frameType(frameType)
    , m_codec(codec)
    , m_channel(channel)
    , m_width(width)
    , m_height(height)
    , m_release(std::move(release))
{
}

/**
 * @brief 析构函数
 *
 * 通过注入的 m_release 回调归还平台码流缓冲区（具体接口由平台决定）。
 */
VideoFrame::~VideoFrame() {
    /* 若释放函数有效则执行释放 */
    if (m_release) {
        m_release();
    }
}

/* FrameFactory 是 VideoFrame 的 friend，能访问私有构造函数；
 * 返回值直接构造，触发默认移动，不发生拷贝 */
VideoFrame FrameFactory::make(const uint8_t* data,
                              size_t         size,
                              int64_t        timestamp,
                              uint32_t       sequence,
                              FrameType      frameType,
                              CodecType      codec,
                              ChannelId      channel,
                              uint16_t       width,
                              uint16_t       height,
                              std::function<void()> release) {
    return VideoFrame(data, size, timestamp, sequence, frameType,
                      codec, channel, width, height, std::move(release));
}

} // namespace camera
