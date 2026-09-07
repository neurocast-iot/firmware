/**
 * @file serial_port.h
 * @brief 串口底层工具类
 *
 * 只管"打开串口、发字节、收字节、超时"这些脏活累活，
 * 不知道上面跑的是什么协议（Zigbee / GPS / 蓝牙都不关心）。
 *
 * 各个传感器 Reader（ZigbeeReader / GpsReader / ...）
 * 拿这个类当底层通道，自己负责"发什么命令、怎么解析响应"。
 *
 * 为什么单独抽出来：
 *   Zigbee / GPS / 蓝牙都是串口通信，打开串口、配波特率、读写字节、select 超时
 *   这些代码完全一样，抽出来只写一次，后面加新传感器不用重复写串口代码。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct termios;  /* POSIX 终端配置结构体，前向声明避免 include 头文件污染 */

namespace nc {
namespace sensor {

class SerialPort {
public:
    /**
     * @param device   串口设备路径（如 "/dev/ttySAK1"）
     * @param baudrate 波特率（如 115200、9600）
     */
    SerialPort(const std::string& device, int baudrate);
    ~SerialPort();

    /* 禁止拷贝（串口 fd 不能复制） */
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    /**
     * 打开串口并配置参数（8N1：8 数据位、无校验、1 停止位）
     * @return 是否成功
     */
    bool open();

    /** 关闭串口（析构时自动调用） */
    void close();

    /** 串口是否已打开 */
    bool isOpen() const;

    /** 获取设备路径（打日志用） */
    const std::string& device() const { return m_device; }

    /**
     * 发送数据
     * @param data 要发的字节
     * @param len  字节数
     * @return 实际发出去的字节数（和 len 不一样说明写失败了）
     */
    int write(const uint8_t* data, int len);

    /**
     * 接收数据（带超时）
     *
     * 用 select 等数据到来，读到数据就放到 outBuffer 里。
     * 如果超时还没数据，返回 0。
     *
     * @param outBuffer   输出：读到的字节
     * @param maxLen      最多读多少字节
     * @param timeoutMs   超时毫秒数
     * @return 实际读到的字节数（0 = 超时，<0 = 出错）
     */
    int read(uint8_t* outBuffer, int maxLen, int timeoutMs);

private:
    /** 配置串口参数（波特率、8N1、原始模式） */
    bool configure();

    std::string m_device;
    int m_baudrate;
    int m_fd;
    bool m_isOpen;
    termios* m_oldTermios;  /* 保存原始终端配置，关闭时恢复 */
};

} // namespace sensor
} // namespace nc
