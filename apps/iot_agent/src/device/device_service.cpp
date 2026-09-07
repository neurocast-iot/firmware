/**
 * @file device_service.cpp
 * @brief 设备服务实现 —— 管理本地硬件设备
 *
 * 当前只管 Zigbee 设备（读 IEEE 地址当设备 ID）。
 * 后续加 GPS / 蓝牙时，在 initialize() 里加初始化，在 shutdown() 里加关闭。
 *
 * 核心功能：getDeviceId()
 *   - 优先用配置里的 device_id（调试用，手动指定）
 *   - 没有就从 Zigbee 串口读 IEEE 地址，拼 -C 后缀当设备 ID
 */

/**
 * Device service implementation — manages local hardware peripherals
 *
 * Currently only manages Zigbee devices (reads IEEE address as device ID).
 * When GPS / Bluetooth are added, extend initialize() and shutdown() accordingly.
 *
 * Core function: getDeviceId()
 *   - Prefers device_id from config (for debugging / manual override)
 *   - Falls back to reading IEEE address from Zigbee serial port, with -C suffix as device ID
 */
#include "device_service.h"
#include "config/agent_config.h"
#include "nc/sensor/zigbee_reader.h"
#include "nc/common/log_utils.h"

namespace iot_agent {

DeviceService::DeviceService() = default;

DeviceService::~DeviceService() {
    shutdown();
}

/**
 * 初始化设备服务
 *
 * 当前只做一件事：记录 Zigbee 串口路径（后续加 GPS / 蓝牙在这里扩展）。
 * 实际的硬件打开操作延迟到 getDeviceId() 里，用完就关。
 */
bool DeviceService::initialize(const AgentConfig& cfg) {
    NC_LOGI("[DeviceService] initializing...");
    NC_LOGI("[DeviceService] zigbee port: {}", cfg.zigbeePort().c_str());

    m_initialized = true;
    NC_LOGI("[DeviceService] initialized");
    return true;
}

/**
 * 获取设备 ID
 *
 * 优先级：
 *   1) 配置里有 device_id → 直接用（调试用，手动指定）
 *   2) 配置里没有 → 从 Zigbee 串口读 IEEE 地址，拼 -C 后缀
 *
 * 为什么拼 -C？
 *   参考 gw_av100 原始逻辑：设备 ID 格式是 "IEEE地址-C"，
 *   TB 那边用这个当 deviceName 和 MQTT clientId。
 *
 * @param cfg iot_agent 配置（可能已经有 device_id）
 * @return 设备 ID 字符串，拿不到返回空
 */
std::string DeviceService::getDeviceId(const AgentConfig& cfg) {
    /* 1) 配置里有 device_id → 直接用 */
    if (!cfg.deviceId().empty()) {
        NC_LOGI("[DeviceService] device_id from config: {}", cfg.deviceId().c_str());
        return cfg.deviceId();
    }

    /* 2) 配置里没有 → 从 Zigbee 读 IEEE 地址 */
    NC_LOGI("[DeviceService] device_id not in config, reading from Zigbee...");
    std::string ieeeAddr = readZigbeeDeviceId(cfg.zigbeePort());
    if (ieeeAddr.empty()) {
        NC_LOGE("[DeviceService] failed to get device_id from Zigbee");
        return "";
    }

    /* 拼 -C 后缀（参考 gw_av100 原始逻辑） */
    std::string deviceId = ieeeAddr + "-C";
    NC_LOGI("[DeviceService] device_id from Zigbee: {}", deviceId.c_str());
    return deviceId;
}

/**
 * 关闭设备服务
 *
 * 后续加 GPS / 蓝牙时，在这里关闭对应的 Connector。
 * 析构函数里也会调这个，确保资源释放。
 */
void DeviceService::shutdown() {
    if (!m_initialized) return;
    NC_LOGI("[DeviceService] shutting down...");
    /* TODO: 后续加 GPS / 蓝牙时，在这里关闭对应的 Connector */
    m_initialized = false;
}

/**
 * 从 Zigbee 串口读 IEEE 地址
 *
 * 流程：打开串口 → 发命令读 IEEE 地址 → 等响应 → 解析 → 关闭串口
 * 超时 3 秒，最多重试 3 次（ZigbeeReader 内部逻辑）。
 *
 * @param zigbeePort 串口路径（如 /dev/ttySAK1）
 * @return IEEE 地址字符串（如 "A4C1380092CFA43E"），失败返回空
 */
std::string DeviceService::readZigbeeDeviceId(const std::string& zigbeePort) {
    nc::sensor::ZigbeeReader zigbee(zigbeePort, 115200);

    /* 打开串口（失败会打日志） */
    if (!zigbee.open()) {
        NC_LOGE("[DeviceService] Zigbee serial port open failed: {}", zigbeePort.c_str());
        return "";
    }

    /* 读 IEEE 地址（超时 3 秒，最多重试 3 次） */
    std::string ieeeAddr = zigbee.getIeeeAddr(3000, 100);

    /* 用完就关，不长期占用串口 */
    zigbee.close();

    if (ieeeAddr.empty()) {
        NC_LOGE("[DeviceService] Zigbee read IEEE addr failed");
        return "";
    }
    return ieeeAddr;
}

} // namespace iot_agent
