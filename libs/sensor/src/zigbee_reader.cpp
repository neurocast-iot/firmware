/**
 * @file zigbee_reader.cpp
 * @brief Zigbee IEEE 地址读取器实现
 *
 * 从 gw_av100 的 ZigbeeReader.cpp 搬过来，底层串口改用 SerialPort。
 * 流程：发命令 → 等响应 → 解析帧 → 失败就重试。
 */
#include "nc/sensor/zigbee_reader.h"
#include "nc/sensor/zigbee_protocol.h"
#include "nc/common/log_utils.h"

#include <cstring>
#include <unistd.h>
#include <sys/time.h>

namespace nc {
namespace sensor {

namespace zp = zigbee_proto;  /* 省得每次写一长串 */

/* ====================================================================
 * 构造 / 析构
 * ==================================================================== */
ZigbeeReader::ZigbeeReader(const std::string& device, int baudrate)
    : m_port(device, baudrate)
    , m_recvPos(0)
{
    memset(m_recvBuf, 0, sizeof(m_recvBuf));
    NC_LOGI("[ZigbeeReader] 创建实例: device={}, baudrate={}", device.c_str(), baudrate);
}

ZigbeeReader::~ZigbeeReader() {
    close();
}

bool ZigbeeReader::open() {
    return m_port.open();
}

void ZigbeeReader::close() {
    m_port.close();
    m_recvPos = 0;
}

bool ZigbeeReader::isOpen() const {
    return m_port.isOpen();
}

/* ====================================================================
 * getIeeeAddr：读 IEEE 地址（主流程）
 *
 * 为什么有重试：Zigbee 模块可能还没启动好，第一次发命令没响应，
 * 等 1 秒再试一次，通常第 2~3 次就能拿到。
 * ==================================================================== */
std::string ZigbeeReader::getIeeeAddr(int timeoutMs, int maxRetries) {
    if (!isOpen()) {
        NC_LOGE("[ZigbeeReader] 串口未打开，无法读取 IEEE 地址");
        return "";
    }

    for (int retry = 0; retry < maxRetries; ++retry) {
        if (retry > 0) {
            NC_LOGI("[ZigbeeReader] 第 {} 次重试...", retry + 1);
            if (!sendGetIeeeAddrCmd()) continue;
        } else {
            NC_LOGI("[ZigbeeReader] 开始读取 IEEE 地址");
            if (!sendGetIeeeAddrCmd()) {
                NC_LOGE("[ZigbeeReader] 发送命令失败");
                return "";
            }
        }

        uint8_t ieeeAddr[zp::IEEE_ADDR_LEN] = {0};
        if (receiveAndParse(timeoutMs, ieeeAddr)) {
            char addrStr[32] = {0};
            if (zp::ieeeAddrToString(ieeeAddr, addrStr, sizeof(addrStr))) {
                NC_LOGI("[ZigbeeReader] 成功读取 IEEE 地址: {}", addrStr);
                return std::string(addrStr);
            }
        }

        /* 最后一次不等了，直接返回 */
        if (retry < maxRetries - 1) {
            usleep(1000000);  /* 等 1 秒再重试 */
        }
    }

    NC_LOGE("[ZigbeeReader] 获取 IEEE 地址失败（已重试 {} 次）", maxRetries);
    return "";
}

/* ====================================================================
 * sendGetIeeeAddrCmd：构建并发送"获取 IEEE 地址"命令帧
 * ==================================================================== */
bool ZigbeeReader::sendGetIeeeAddrCmd() {
    uint8_t frame[zp::FRAME_MAX_SIZE] = {0};
    uint16_t frameLen = zp::buildGetIeeeAddrFrame(frame, sizeof(frame));
    if (frameLen == 0) {
        NC_LOGE("[ZigbeeReader] 构建命令帧失败");
        return false;
    }

    int written = m_port.write(frame, frameLen);
    if (written != static_cast<int>(frameLen)) {
        NC_LOGE("[ZigbeeReader] 发送数据失败: 期望={}, 实际={}", frameLen, written);
        return false;
    }
    return true;
}

/* ====================================================================
 * receiveAndParse：等串口数据，攒到缓冲区，解析出 IEEE 地址帧
 *
 * 串口数据可能分好几包到（一包可能只有几个字节），
 * 所以不能"读一次就解析"，要攒在缓冲区里，够了再拼帧。
 * ==================================================================== */
bool ZigbeeReader::receiveAndParse(int timeoutMs, uint8_t* ieeeAddr) {
    if (!ieeeAddr) return false;

    /* 记录开始时间，用来算"总共等了多久" */
    struct timeval startTime;
    gettimeofday(&startTime, nullptr);
    long totalTimeoutUs = static_cast<long>(timeoutMs) * 1000;

    while (true) {
        /* 算一下还剩多少时间 */
        struct timeval now;
        gettimeofday(&now, nullptr);
        long elapsedUs = (now.tv_sec - startTime.tv_sec) * 1000000L
                       + (now.tv_usec - startTime.tv_usec);
        long remainUs = totalTimeoutUs - elapsedUs;
        if (remainUs <= 0) return false;

        /* 用 select 等数据到来（SerialPort::read 内部用 select） */
        uint8_t tmp[256] = {0};
        int bytesRead = m_port.read(tmp, sizeof(tmp), static_cast<int>(remainUs / 1000));
        if (bytesRead < 0) return false;
        if (bytesRead == 0) continue;  /* 超时，再检查总超时 */

        /* 把新数据追加到接收缓冲区 */
        for (int i = 0; i < bytesRead; ++i) {
            if (m_recvPos < static_cast<int>(sizeof(m_recvBuf))) {
                m_recvBuf[m_recvPos++] = tmp[i];
            } else {
                /* 缓冲区满了，从头覆盖（旧数据不要了） */
                m_recvPos = 0;
                m_recvBuf[m_recvPos++] = tmp[i];
            }
        }

        /* 内层循环：缓冲区里可能有多个帧，一个个处理 */
        while (m_recvPos >= static_cast<int>(zp::FRAME_MIN_SIZE)) {
            int frameLen = 0;
            if (!findCompleteFrame(frameLen)) break;  /* 没有完整帧，继续等 */

            /* 校验 checksum */
            bool checksumOk = verifyChecksum(m_recvBuf, frameLen);

            /* 读消息类型，判断是不是 IEEE 地址响应 */
            uint16_t msgType = zp::makeWord(
                m_recvBuf[zp::OFF_MSG_TYPE_HI], m_recvBuf[zp::OFF_MSG_TYPE_LO]);

            if (!checksumOk && msgType != zp::CMD_GET_IEEE_ADDR_RSP) {
                /* 校验和不对，而且不是 IEEE 地址帧（IEEE 地址帧允许校验和失败），跳过 */
                memmove(m_recvBuf, &m_recvBuf[1], m_recvPos - 1);
                m_recvPos -= 1;
                continue;
            }

            /* 尝试解析 IEEE 地址 */
            if (zp::parseIeeeAddrResponse(m_recvBuf, frameLen, ieeeAddr)) {
                return true;
            }

            /* 不是目标帧，跳过整个帧 */
            memmove(m_recvBuf, &m_recvBuf[frameLen], m_recvPos - frameLen);
            m_recvPos -= frameLen;
        }
    }
    return false;
}

/* ====================================================================
 * findCompleteFrame：从缓冲区里找一个完整帧
 *
 * 找帧头 0x55 → 读 payload 长度 → 检查帧尾 0xAA → 确认帧完整
 * ==================================================================== */
bool ZigbeeReader::findCompleteFrame(int& frameLen) {
    if (m_recvPos < static_cast<int>(zp::FRAME_MIN_SIZE)) return false;

    /* 找帧头 0x55 */
    int startIdx = -1;
    for (int i = 0; i <= m_recvPos - static_cast<int>(zp::FRAME_MIN_SIZE); ++i) {
        if (m_recvBuf[i] == zp::FRAME_HEADER) {
            startIdx = i;
            break;
        }
    }
    if (startIdx < 0) return false;

    int remaining = m_recvPos - startIdx;
    if (remaining < static_cast<int>(zp::FRAME_MIN_SIZE)) return false;

    /* 读 payload 长度，算完整帧总长度 */
    uint16_t payloadLen = zp::makeWord(
        m_recvBuf[startIdx + zp::OFF_LEN_HI],
        m_recvBuf[startIdx + zp::OFF_LEN_LO]);
    frameLen = zp::OFF_PAYLOAD + payloadLen + 1;  /* +1 是帧尾 */

    if (remaining >= frameLen && m_recvBuf[startIdx + frameLen - 1] == zp::FRAME_TAIL) {
        /* 找到完整帧，把帧头前面的垃圾数据丢掉 */
        if (startIdx > 0) {
            memmove(m_recvBuf, &m_recvBuf[startIdx], m_recvPos - startIdx);
            m_recvPos -= startIdx;
        }
        return true;
    }
    return false;
}

/* ====================================================================
 * verifyChecksum：校验帧的 checksum
 *
 * checksum 计算范围：从 MessageType 到 Payload（跳过 Checksum 字节本身）
 * 分两段加：[1..4]（MsgType + Length）+ [6..frameLen-2]（Payload）
 * ==================================================================== */
bool ZigbeeReader::verifyChecksum(const uint8_t* frame, int frameLen) {
    if (!frame || frameLen < static_cast<int>(zp::FRAME_MIN_SIZE)) return false;

    uint8_t sum = 0;
    /* 第一段：MsgType(2) + Length(2) = frame[1]~frame[4] */
    for (int i = 1; i <= 4; ++i) sum += frame[i];
    /* 第二段：Payload = frame[6]~frame[frameLen-2]（跳过 Checksum 字节 frame[5]） */
    for (int i = 6; i < frameLen - 1; ++i) sum += frame[i];

    return sum == frame[zp::OFF_CHECKSUM];
}

} // namespace sensor
} // namespace nc
