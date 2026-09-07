/**
 * @file bluetooth_trigger.h
 * @brief 蓝牙标签触发源
 *
 * 检测到蓝牙标签（手环）进入范围时触发拍照。
 * 底层用 BtTagReader 从串口持续采集蓝牙数据。
 *
 * 拍照模型（对齐旧项目 gw_av100）：
 *   检测到标签 → 加入缓存 → 唤醒拍照线程
 *   拍照线程循环：有标签就拍一张 → 复制给所有标签 → count++ → 满数踢出
 *   一张照片服务所有标签，缓存空了才停。
 *
 * 线程模型：
 *   - 采集线程（BtTagReader）：串口读取 + 解析，发现标签调 onTagDetected
 *   - 拍照线程（m_photoThread）：从缓存取标签 → 拍照 → 复制 → 计数
 *   - onTagDetected 只做加缓存 + 通知，不阻塞采集
 */
#pragma once

#include "trigger.h"

#include "nc/sensor/bt_tag_reader.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace mediad {

class BluetoothTrigger : public Trigger {
public:
    /** 蓝牙硬件参数（设备固定，不需要配置） */
    static constexpr const char* BT_SERIAL_PORT = "/dev/ttySAK2";
    static constexpr int BT_BAUDRATE = 9600;

    /** 蓝牙触发源配置 */
    struct Config {
        std::string id;
        int priority = 30;                       ///< 优先级（蓝牙默认 30）
        int burstCount = 1;                      ///< 每个标签需要的照片数
        int burstIntervalMs = 0;                 ///< 连拍间隔（毫秒）
    };

    explicit BluetoothTrigger(const Config& cfg);
    ~BluetoothTrigger() override;

    BluetoothTrigger(const BluetoothTrigger&) = delete;
    BluetoothTrigger& operator=(const BluetoothTrigger&) = delete;

    void enable() override;
    void disable() override;
    bool isEnabled() const override;

    const char* getId() const override { return m_id.c_str(); }

    /** 获取当前配置（用于增量更新时对比） */
    TriggerCfg getConfig() const override;

private:
    /** 缓存中的标签：记录已拍张数 + 需要总数 */
    struct TagEntry {
        int count;       ///< 已拍张数
        int burstCount;  ///< 需要总数（来自配置）
    };

    /**
     * 标签检测回调（在采集线程上下文执行）
     *
     * 只做两件事：加入缓存、唤醒拍照线程。不阻塞采集。
     */
    void onTagDetected(const nc::sensor::BtTagInfo& tag);

    /**
     * 拍照线程主循环
     *
     * 缓存不空就一直拍：拍一张 → 复制给所有标签 → count++ → 踢满数的。
     * 缓存空了就休眠等条件变量唤醒。
     */
    void photoLoop();

    /**
     * 把照片文件复制一份给指定标签
     *
     * @param srcPath 拍照生成的源文件路径
     * @param tagId 标签 ID
     * @return 复制后的文件路径，失败返回空串
     */
    std::string copyPhotoForTag(const std::string& srcPath, const std::string& tagId);

    std::string m_id;
    Config m_cfg;
    std::unique_ptr<nc::sensor::BtTagReader> m_reader;

    /* 标签缓存 + 拍照线程同步 */
    std::mutex m_cacheMutex;
    std::unordered_map<std::string, TagEntry> m_tagCache;
    std::condition_variable m_photoCv;
    std::thread m_photoThread;
    std::atomic<bool> m_running{false};

    std::atomic<bool> m_enabled{false};
};

} // namespace mediad
