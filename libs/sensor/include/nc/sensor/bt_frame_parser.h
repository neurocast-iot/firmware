/**
 * @file bt_frame_parser.h
 * @brief 蓝牙帧解析器
 *
 * 从串口字节流里识别完整的蓝牙帧，提取标签 ID 和状态。
 * 不知道上层业务（不关心拍照、不关心 TriggerManager），
 * 只负责"串口收到了一堆字节 → 拼成帧 → 校验 → 提取有效信息"。
 *
 * 帧格式：
 *   帧头(0x55) + 长度(2字节,大端) + 载荷 + 校验和(1字节) + 帧尾(0xAA)
 *   长度 = 载荷字节数
 *   校验和 = 载荷所有字节累加
 *
 * 当前只处理手环命令（commandType=0x1A），
 * 并且只返回"进入灯杆范围"（status bit2=1）的标签。
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nc {
namespace sensor {

/** 蓝牙标签解析结果 */
struct BtTagInfo {
    std::string tagId;       ///< 标签 ID（6 字节 hex，如 "a1b2c3d4e5f6"）
    std::string macAddress;  ///< 蓝牙 MAC 地址（8 字节，冒号分隔）
    uint8_t status;          ///< 状态位：bit0=跌倒 bit1=SOS bit2=进入灯杆范围
    uint8_t deviceType;      ///< 设备类型
    uint8_t commandType;     ///< 命令类型（0x1A=手环）

    /** 是否进入了灯杆范围（bit2=1） */
    bool isInRange() const { return (status & 0x04) != 0; }
};

/**
 * 蓝牙帧解析器
 *
 * 用法：
 *   1. 串口读到数据后调 appendData() 喂进去
 *   2. 循环调 parseNext()，返回有值说明解析出一个完整帧
 *   3. 返回空说明缓冲区里还没有完整帧，等更多数据
 *
 * 内部用环形缓冲区攒数据，支持一帧一帧地解析。
 */
class BtFrameParser {
public:
    static constexpr uint8_t  FRAME_HEADER     = 0x55;   ///< 帧头
    static constexpr uint8_t  FRAME_TAIL       = 0xAA;   ///< 帧尾
    static constexpr uint8_t  CMD_BRACELET     = 0x1A;   ///< 手环命令类型
    static constexpr uint16_t MAX_FRAME_LENGTH = 512;    ///< 单帧最大长度

    BtFrameParser();

    /** 把串口读到的字节追加到内部缓冲区 */
    void appendData(const uint8_t* data, int len);

    /**
     * 尝试解析下一个完整的蓝牙帧
     *
     * @return 解析成功返回 BtTagInfo（只返回进入范围的标签），
     *         返回空字符串的 tagId 表示缓冲区里还没有完整帧
     */
    BtTagInfo parseNext();

    /** 清空缓冲区，重置解析状态 */
    void reset();

private:
    /** 解析一个完整帧，提取标签信息 */
    BtTagInfo parseFrame(const std::vector<uint8_t>& frame);

    /** 计算校验和：data[start] 到 data[end-1] 所有字节累加 */
    uint8_t calcChecksum(const uint8_t* data, int start, int end);

    static constexpr size_t RING_BUF_SIZE = 1024;
    std::array<uint8_t, RING_BUF_SIZE> m_ringBuf;
    size_t m_readPos;   ///< 写入位置（下一个字节放这里）
    size_t m_parsePos;  ///< 解析位置（下一个要解析的字节在这里）
};

} // namespace sensor
} // namespace nc
