/**
 * @file bt_frame_parser.cpp
 * @brief 蓝牙帧解析器实现
 *
 * 环形缓冲区攒字节 → 找帧头 0x55 → 读长度 → 等帧尾 0xAA → 校验 → 提取标签信息。
 * 串口数据可能分多次到（一次只有几个字节），所以不能"读一次就解析"，
 * 要先攒在环形缓冲区里，够了再拼帧。
 */
#include "nc/sensor/bt_frame_parser.h"
#include "nc/common/log_utils.h"

#include <cstdio>
#include <cstring>

namespace nc {
namespace sensor {

BtFrameParser::BtFrameParser()
    : m_readPos(0)
    , m_parsePos(0)
{
    m_ringBuf.fill(0);
}

void BtFrameParser::appendData(const uint8_t* data, int len) {
    for (int i = 0; i < len; ++i) {
        m_ringBuf[m_readPos] = data[i];
        m_readPos = (m_readPos + 1) % RING_BUF_SIZE;
    }
}

BtTagInfo BtFrameParser::parseNext() {
    BtTagInfo empty;  /* tagId 为空表示没解析出帧 */

    while (m_parsePos != m_readPos) {
        /* 至少需要 3 字节才能读帧头 + 长度 */
        size_t unparseBytes = (m_readPos + RING_BUF_SIZE - m_parsePos) % RING_BUF_SIZE;
        if (unparseBytes < 3) {
            break;
        }

        /* 找帧头 0x55 */
        if (m_ringBuf[m_parsePos] != FRAME_HEADER) {
            m_parsePos = (m_parsePos + 1) % RING_BUF_SIZE;
            continue;
        }

        /* 读长度字段（2 字节，大端序） */
        uint16_t lenHi = m_ringBuf[(m_parsePos + 1) % RING_BUF_SIZE];
        uint16_t lenLo = m_ringBuf[(m_parsePos + 2) % RING_BUF_SIZE];
        uint16_t payloadLen = static_cast<uint16_t>((lenHi << 8) | lenLo);

        if (payloadLen > MAX_FRAME_LENGTH) {
            m_parsePos = (m_parsePos + 1) % RING_BUF_SIZE;
            continue;
        }

        /* 完整帧长度 = 帧头(1) + 长度(2) + 载荷(payloadLen) + 校验和(1) + 帧尾(1) */
        size_t frameLen = static_cast<size_t>(payloadLen) + 5;
        if (unparseBytes < frameLen) {
            break;  /* 数据不完整，等更多数据到来 */
        }

        /* 验证帧尾 0xAA */
        if (m_ringBuf[(m_parsePos + frameLen - 1) % RING_BUF_SIZE] != FRAME_TAIL) {
            m_parsePos = (m_parsePos + 1) % RING_BUF_SIZE;
            continue;
        }

        /* 提取完整帧到连续内存，方便后续解析 */
        std::vector<uint8_t> frame(frameLen);
        for (size_t i = 0; i < frameLen; ++i) {
            frame[i] = m_ringBuf[(m_parsePos + i) % RING_BUF_SIZE];
        }

        /* 跳过已解析的帧 */
        m_parsePos = (m_parsePos + frameLen) % RING_BUF_SIZE;

        /* 解析帧内容 */
        BtTagInfo tag = parseFrame(frame);
        if (!tag.tagId.empty()) {
            return tag;
        }
    }

    return empty;
}

void BtFrameParser::reset() {
    m_ringBuf.fill(0);
    m_readPos = 0;
    m_parsePos = 0;
}

/**
 * 解析单个帧，提取标签信息
 *
 * 帧布局（payload 部分，从 frame[3] 开始）：
 *   [0]     addrMode
 *   [1..8]  MAC 地址（8 字节）
 *   [9]     deviceType
 *   [10]    commandType
 *   [11]    保留
 *   [12..17] tag ID（6 字节）
 *   [22]    status（状态位）
 *
 * 只处理手环命令（0x1A），只返回进入范围的标签（bit2=1）。
 */
BtTagInfo BtFrameParser::parseFrame(const std::vector<uint8_t>& frame) {
    BtTagInfo empty;

    /* 最小帧长度：帧头(1) + 长度(2) + 最小载荷(23) + 校验和(1) + 帧尾(1) = 28 */
    if (frame.size() < 28) {
        return empty;
    }

    /* 载荷起始位置 = 帧头(1) + 长度(2) = 3 */
    size_t payloadStart = 3;
    size_t payloadLen = frame.size() - 5;  /* 去掉帧头、长度、校验和、帧尾 */

    if (payloadLen < 23) {
        return empty;
    }

    const uint8_t* payload = &frame[payloadStart];

    /* 校验和验证：载荷所有字节累加 == 校验和字节 */
    uint8_t expectedChecksum = calcChecksum(payload, 0, static_cast<int>(payloadLen));
    if (expectedChecksum != frame[frame.size() - 2]) {
        NC_LOGW("[BtFrameParser] 校验和错误: expected=0x{:02X}, actual=0x{:02X}",
                expectedChecksum, frame[frame.size() - 2]);
        return empty;
    }

    uint8_t commandType = payload[10];

    /* 只处理手环命令 */
    if (commandType != CMD_BRACELET) {
        return empty;
    }

    BtTagInfo info;
    info.commandType = commandType;
    info.deviceType = payload[9];

    /* MAC 地址（8 字节，冒号分隔） */
    char mac[32];
    std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                  payload[1], payload[2], payload[3], payload[4],
                  payload[5], payload[6], payload[7], payload[8]);
    info.macAddress = mac;

    /* 标签 ID（6 字节，连续 hex） */
    char tagId[16];
    std::snprintf(tagId, sizeof(tagId), "%02x%02x%02x%02x%02x%02x",
                  payload[12], payload[13], payload[14],
                  payload[15], payload[16], payload[17]);
    info.tagId = tagId;

    /* 状态字节 */
    info.status = payload[22];

    /* 只返回进入灯杆范围的标签（bit2=1），其他丢弃 */
    if (!info.isInRange()) {
        return empty;
    }

    return info;
}

uint8_t BtFrameParser::calcChecksum(const uint8_t* data, int start, int end) {
    uint8_t sum = 0;
    for (int i = start; i < end; ++i) {
        sum += data[i];
    }
    return sum;
}

} // namespace sensor
} // namespace nc
