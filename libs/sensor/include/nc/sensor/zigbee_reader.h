/**
 * @file zigbee_reader.h
 * @brief Zigbee IEEE 地址读取器
 *
 * 通过串口和 Zigbee 模块通信，读取模块的 IEEE 地址（当设备 ID 用）。
 * 底层用 SerialPort 收发字节，自己只负责"发 Zigbee 命令 + 解析 Zigbee 帧"。
 *
 * 典型用法：
 *   ZigbeeReader zb("/dev/ttySAK1", 115200);
 *   if (zb.open()) {
 *       std::string id = zb.getIeeeAddr(3000);  // 超时 3 秒
 *       zb.close();
 *       // id = "A1B2C3D4E5F60708"
 *   }
 */
#pragma once

#include <string>
#include <cstdint>
#include "nc/sensor/serial_port.h"

namespace nc {
namespace sensor {

class ZigbeeReader {
public:
    /**
     * @param device   Zigbee 模块的串口路径（如 "/dev/ttySAK1"）
     * @param baudrate 波特率（默认 115200）
     */
    explicit ZigbeeReader(const std::string& device, int baudrate = 115200);
    ~ZigbeeReader();

    /* 禁止拷贝 */
    ZigbeeReader(const ZigbeeReader&) = delete;
    ZigbeeReader& operator=(const ZigbeeReader&) = delete;

    /** 打开串口 */
    bool open();

    /** 关闭串口 */
    void close();

    /** 串口是否已打开 */
    bool isOpen() const;

    /**
     * 读取 Zigbee 模块的 IEEE 地址
     *
     * 流程：发命令 → 等响应 → 解析帧 → 重试（最多 maxRetries 次）
     *
     * @param timeoutMs  单次等待超时（毫秒），默认 3000
     * @param maxRetries 最多重试几次，默认 3
     * @return IEEE 地址字符串（16 位十六进制），失败返回空字符串
     */
    std::string getIeeeAddr(int timeoutMs = 3000, int maxRetries = 3);

private:
    /** 发送"获取 IEEE 地址"命令 */
    bool sendGetIeeeAddrCmd();

    /**
     * 等待并解析响应
     * @param timeoutMs 超时
     * @param ieeeAddr  输出 8 字节地址
     * @return 是否成功
     */
    bool receiveAndParse(int timeoutMs, uint8_t* ieeeAddr);

    /**
     * 从接收缓冲区里找一个完整的 Zigbee 帧
     * @param frameLen 输出：找到的帧长度
     * @return 是否找到
     */
    bool findCompleteFrame(int& frameLen);

    /** 校验帧的 checksum 对不对 */
    bool verifyChecksum(const uint8_t* frame, int frameLen);

    SerialPort m_port;

    /* 接收缓冲区：串口数据可能分多次到，先攒起来再拼帧 */
    uint8_t m_recvBuf[256];
    int m_recvPos;
};

} // namespace sensor
} // namespace nc
