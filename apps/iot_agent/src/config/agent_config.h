/**
 * @file agent_config.h
 * @brief iot_agent 自身配置
 *
 * 负责读 /etc/iot_agent.json（路径可改），把整个 JSON 原样保留成字符串，
 * 交给平台实现自己解析（不同平台需要的字段不一样，不在这里定死）。
 *
 * 同时提供几个"平台无关"的通用字段访问（如 token 文件路径、日志级别），
 * 这些字段所有平台都用得到。
 */
#pragma once

#include <string>

namespace iot_agent {

class AgentConfig {
public:
    /**
     * 从文件加载配置
     * @param path 配置文件路径（默认 /etc/iot_agent.json）
     * @return 是否加载成功（文件不存在 / 解析失败返回 false）
     */
    bool load(const std::string& path);

    /** 整个配置 JSON 原样字符串（交给平台实现自己解析） */
    const std::string& rawJson() const { return m_rawJson; }

    /** 平台名（"thingsboard" / "emqx" / ...），从配置里读出来 */
    const std::string& platformName() const { return m_platformName; }

    /** token 持久化路径（/data/iot_agent/token.json） */
    std::string tokenPath() const { return m_dataDir + "/token.json"; }

    /** 设备配置路径（/data/iot_agent/device_config.json） */
    std::string configPath() const { return m_dataDir + "/device_config.json"; }

    /**
     * 恢复出厂配置：删掉配置文件和 token
     * 下次启动会重新走 provision 拿凭据、重新从云端同步配置
     */
    bool reset();

    /**
     * 运行时数据目录（/data/iot_agent/）
     * 嵌入式设备上 /data/ 是可读写分区，放 token、业务配置等运行时数据
     */
    const std::string& dataDir() const { return m_dataDir; }

    /**
     * 设备 ID（用于 MQTT clientId 和 TB 认证）
     * 来源优先级：配置文件里的 device_id 字段 > 从 Zigbee 串口读 IEEE 地址
     */
    const std::string& deviceId() const { return m_deviceId; }
    void setDeviceId(const std::string& id) { m_deviceId = id; }

    /** Zigbee 串口路径（默认 /dev/ttySAK1），读不到设备 ID 时用来走 Zigbee */
    const std::string& zigbeePort() const { return m_zigbeePort; }

    /** OTA 基础目录（默认 /mnt/emmc/ota），从配置文件的 ota.base_dir 字段读 */
    const std::string& otaBaseDir() const { return m_otaBaseDir; }

private:
    std::string m_rawJson;
    std::string m_platformName = "thingsboard";   /* 默认对接 TB */
    std::string m_dataDir      = "/data/iot_agent";   /* 运行时数据目录 */
    std::string m_deviceId;                        /* 设备 ID（配置里读或 Zigbee 读） */
    std::string m_zigbeePort   = "/dev/ttySAK1";  /* Zigbee 串口路径 */
    std::string m_otaBaseDir   = "/mnt/emmc/ota";   /* OTA 基础目录 */
};

} // namespace iot_agent
