/**
 * @file zigbee_protocol.h
 * @brief Zigbee 通信协议常量和工具函数
 *
 * 从 gw_av100 的 ZigbeeProtocol.h 搬过来的，协议定义不变：
 *   帧结构: Header(0x55) + MessageType(2字节) + Length(2字节) + Checksum(1字节) + Payload + Tail(0xAA)
 *   字节序: 大端序（高字节在前）
 *   校验和: 除 Header 和 Tail 外所有字节的累加和
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>

namespace nc {
namespace sensor {
namespace zigbee_proto {

/* ---- 命令码 ---- */
const uint16_t CMD_GET_IEEE_ADDR     = 0x01C0;   /* 主机 → Zigbee：获取 IEEE 地址 */
const uint16_t CMD_GET_IEEE_ADDR_RSP = 0x81C0;   /* Zigbee → 主机：IEEE 地址响应 */
const uint16_t CMD_ACKNOWLEDGE       = 0x8000;   /* 确认响应 */

/* ---- 帧结构常量 ---- */
const uint8_t  FRAME_HEADER   = 0x55;
const uint8_t  FRAME_TAIL     = 0xAA;
const uint16_t FRAME_MAX_SIZE = 256;
const uint16_t FRAME_MIN_SIZE = 7;   /* Header(1)+MsgType(2)+Len(2)+Checksum(1)+Tail(1) */
const uint16_t IEEE_ADDR_LEN  = 8;

/* ---- 字段偏移 ---- */
const uint16_t OFF_HEADER       = 0;
const uint16_t OFF_MSG_TYPE_HI  = 1;
const uint16_t OFF_MSG_TYPE_LO  = 2;
const uint16_t OFF_LEN_HI       = 3;
const uint16_t OFF_LEN_LO       = 4;
const uint16_t OFF_CHECKSUM     = 5;
const uint16_t OFF_PAYLOAD      = 6;

/* ---- 字节序工具 ---- */
inline uint8_t  hiByte(uint16_t v)  { return static_cast<uint8_t>((v >> 8) & 0xFF); }
inline uint8_t  loByte(uint16_t v)  { return static_cast<uint8_t>(v & 0xFF); }
inline uint16_t makeWord(uint8_t h, uint8_t l) { return static_cast<uint16_t>((h << 8) | l); }

/* ---- 校验和：从 start 到 end（不含）所有字节累加 ---- */
inline uint8_t calcChecksum(const uint8_t* data, int start, int end) {
    uint8_t sum = 0;
    for (int i = start; i < end; ++i) sum += data[i];
    return sum;
}

/**
 * 构建"获取 IEEE 地址"命令帧
 * @return 帧长度，失败返回 0
 */
inline uint16_t buildGetIeeeAddrFrame(uint8_t* buf, uint16_t bufSize) {
    if (!buf || bufSize < FRAME_MIN_SIZE) return 0;
    int off = 0;
    buf[off++] = FRAME_HEADER;
    buf[off++] = hiByte(CMD_GET_IEEE_ADDR);
    buf[off++] = loByte(CMD_GET_IEEE_ADDR);
    buf[off++] = 0;  /* payload 长度高字节 */
    buf[off++] = 0;  /* payload 长度低字节 */
    buf[off++] = 0;  /* checksum 占位 */
    buf[off++] = FRAME_TAIL;
    /* checksum = MsgType(2) + Length(2) 的累加 */
    buf[OFF_CHECKSUM] = calcChecksum(buf, 1, off - 1);
    return static_cast<uint16_t>(off);
}

/**
 * 解析 IEEE 地址响应帧
 * @return true = 解析成功，ieeeAddr 里放了 8 字节地址
 */
inline bool parseIeeeAddrResponse(const uint8_t* frame, uint16_t frameLen, uint8_t* ieeeAddr) {
    if (!frame || !ieeeAddr || frameLen < FRAME_MIN_SIZE) return false;
    if (frame[OFF_HEADER] != FRAME_HEADER) return false;
    uint16_t msgType = makeWord(frame[OFF_MSG_TYPE_HI], frame[OFF_MSG_TYPE_LO]);
    if (msgType != CMD_GET_IEEE_ADDR_RSP) return false;
    uint16_t payloadLen = makeWord(frame[OFF_LEN_HI], frame[OFF_LEN_LO]);
    if (payloadLen < IEEE_ADDR_LEN) return false;
    memcpy(ieeeAddr, &frame[OFF_PAYLOAD], IEEE_ADDR_LEN);
    return true;
}

/**
 * 把 8 字节 IEEE 地址转成十六进制字符串
 * @return true = 成功（全零地址返回 false）
 */
inline bool ieeeAddrToString(const uint8_t* addr, char* out, int outSize) {
    if (!addr || !out || outSize < 17) return false;
    bool allZero = true;
    for (int i = 0; i < IEEE_ADDR_LEN; ++i) {
        if (addr[i] != 0) { allZero = false; break; }
    }
    if (allZero) return false;
    snprintf(out, outSize, "%02X%02X%02X%02X%02X%02X%02X%02X",
             addr[0], addr[1], addr[2], addr[3],
             addr[4], addr[5], addr[6], addr[7]);
    return true;
}

} // namespace zigbee_proto
} // namespace sensor
} // namespace nc
