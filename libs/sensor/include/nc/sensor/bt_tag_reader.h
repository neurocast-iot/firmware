/**
 * @file bt_tag_reader.h
 * @brief 蓝牙标签采集器
 *
 * 开一个后台线程，持续从串口读蓝牙数据，用 BtFrameParser 解析帧，
 * 每解析出一个有效标签就通过回调通知上层。
 *
 * 职责边界：
 *   - 只管"采集 + 解析 + 回调"
 *   - 不知道去重、不知道拍照、不知道 TriggerManager
 *   - 去重和业务逻辑由上层（BluetoothTrigger）负责
 *
 * 和 ZigbeeReader 的区别：
 *   - ZigbeeReader 是"发命令 → 等响应"（一次性查询）
 *   - BtTagReader 是"持续监听 → 有数据就回调"（后台采集）
 */
#pragma once

#include "nc/sensor/serial_port.h"
#include "nc/sensor/bt_frame_parser.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace nc {
namespace sensor {

/**
 * 蓝牙标签采集器
 *
 * 用法：
 *   BtTagReader reader("/dev/ttySAK2", 9600);
 *   reader.setTagCallback([](const BtTagInfo& tag) {
 *       // 处理新标签
 *   });
 *   reader.start();   // 开始采集
 *   // ...
 *   reader.stop();    // 停止采集
 */
class BtTagReader {
public:
    /** 标签回调类型 */
    using TagCallback = std::function<void(const BtTagInfo&)>;

    /**
     * @param device   蓝牙模块的串口路径（如 "/dev/ttySAK2"）
     * @param baudrate 波特率（默认 9600，蓝牙模块常用波特率）
     */
    explicit BtTagReader(const std::string& device, int baudrate = 9600);
    ~BtTagReader();

    /* 禁止拷贝 */
    BtTagReader(const BtTagReader&) = delete;
    BtTagReader& operator=(const BtTagReader&) = delete;

    /** 设置标签回调（解析出有效标签时调用） */
    void setTagCallback(TagCallback callback);

    /**
     * 启动采集线程
     *
     * 打开串口 → 创建后台线程 → 循环读串口 + 解析 + 回调。
     * 串口打开失败返回 false。
     */
    bool start();

    /**
     * 停止采集线程
     *
     * 设置停止标志 → 等线程退出（join）→ 关闭串口。
     * 幂等：未启动时调用无副作用。
     */
    void stop();

    /** 是否正在采集 */
    bool isRunning() const;

private:
    /**
     * 采集线程主循环
     *
     * 读串口 → 喂给解析器 → 循环调 parseNext() 直到没有完整帧 →
     * 每个解析出的标签通过回调发出去 → sleep 一下再读。
     */
    void collectLoop();

    std::string m_device;
    int m_baudrate;
    SerialPort m_port;
    BtFrameParser m_parser;
    TagCallback m_callback;

    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

} // namespace sensor
} // namespace nc
