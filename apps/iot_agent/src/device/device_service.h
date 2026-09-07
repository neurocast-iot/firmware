/**
 * @file device_service.h
 * @brief 设备服务 —— 管理所有硬件设备（Zigbee / GPS / 蓝牙 ...）
 *
 * 参考 ThingsBoard IoT Gateway 的 Connector 层：
 *   每种硬件设备对应一个 Connector，DeviceService 统一管理它们的生命周期。
 *
 * 职责：
 *   1) 初始化硬件设备（打开串口、建立连接）
 *   2) 获取设备 ID（从 Zigbee 读 IEEE 地址）
 *   3) 后续扩展：GPS 定位、蓝牙扫描等
 *
 * 和 CloudService 的分工：
 *   DeviceService 管“本地硬件”，CloudService 管“远端云端”。
 */

/**
 * Device service — manages all hardware peripherals (Zigbee / GPS / Bluetooth ...)
 *
 * Inspired by ThingsBoard IoT Gateway's Connector layer:
 *   each hardware peripheral maps to a Connector; DeviceService manages their lifecycle.
 *
 * Responsibilities:
 *   1) Initialize hardware peripherals (open serial ports, establish connections)
 *   2) Obtain device ID (read IEEE address from Zigbee)
 *   3) Future extensions: GPS positioning, Bluetooth scanning, etc.
 *
 * Division of labor with CloudService:
 *   DeviceService manages "local hardware"; CloudService manages "remote cloud".
 */
#pragma once

#include <memory>
#include <string>

namespace nc { namespace sensor { class ZigbeeReader; } }

namespace iot_agent {

class AgentConfig;

class DeviceService {
public:
    DeviceService();
    ~DeviceService();

    /**
     * 初始化设备服务
     *
     * 根据配置里的设备信息，初始化对应的硬件连接器：
     *   - Zigbee：打开串口，准备读 IEEE 地址
     *   - GPS / 蓝牙：后续加
     *
     * @param cfg iot_agent 配置
     * @return 是否成功
     */
    bool initialize(const AgentConfig& cfg);

    /**
     * 获取设备 ID
     *
     * 优先用配置里的 device_id，没有就从 Zigbee 读 IEEE 地址拼 -C 后缀。
     * 如果配置里没有且 Zigbee 也读不到，返回空字符串。
     *
     * @param cfg iot_agent 配置（可能已经有 device_id）
     * @return 设备 ID 字符串
     */
    std::string getDeviceId(const AgentConfig& cfg);

    /** 关闭所有硬件设备 */
    void shutdown();

private:
    /** 从 Zigbee 串口读 IEEE 地址作为设备 ID */
    std::string readZigbeeDeviceId(const std::string& zigbeePort);

    /* 后续扩展：
     * std::unique_ptr<GpsConnector> m_gps;
     * std::unique_ptr<BluetoothConnector> m_bluetooth;
     */
    bool m_initialized = false;
};

} // namespace iot_agent
