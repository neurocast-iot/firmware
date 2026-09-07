/**
 * @file config_router.h
 * @brief 配置管理器（参考 gw_av100 的 ConfigManager）
 *
 * 职责：
 *   1) 收到 MQTT 推送的属性 JSON → 合并到本地配置 → 存文件
 *   2) 启动时从本地文件加载配置（设备重启后不丢配置）
 *   3) 提供配置读取接口（其他模块需要时来取）
 *
 * 注意：OTA 通知结构体 OtaNotice 已挪到 ota/ota_types.h，
 *       这里只管配置合并，不再定义 OTA 相关类型。
 *
 * 和原始 ConfigManager 的区别：
 *   原始是单进程，配置变了直接 subscriber 回调通知相机/录制模块重启。
 *   现在 iot_agent 和 mediad 是两个进程，配置变了要先存本地，
 *   后续再通过 IPC 通知 mediad（这一步后面再加）。
 */
#pragma once

#include <functional>
#include <mutex>
#include <string>

namespace iot_agent {

class ConfigRouter {
public:
    ConfigRouter();
    ~ConfigRouter();

    /**
     * 初始化：从本地文件加载配置
     *
     * @param configPath 配置文件路径（默认 /data/iot_agent/device_config.json）
     *                    嵌入式设备上 /data/ 是可读写分区，业务配置放这里
     * @return 是否成功（文件不存在也算成功，等联网后从 TB 同步）
     */
    bool initialize(const std::string& configPath = "/data/iot_agent/device_config.json");

    /**
     * 处理 MQTT 推送的属性 JSON
     *
     * 参考原始 DeviceManager::parseSharedAttributes：
     *   1) 把业务配置字段和当前值比较，只合并真正变了的字段
     *   2) 如果有变更，存到本地文件
     *
     * 注意：OTA 字段（fw_* / sw_*）已在 TBAdapter::parseMessage() 里检测，
     *       这里不再处理，只负责配置合并。
     *
     * @param attributesJson 原始 JSON 字符串（TB 推送的 shared 属性）
     * @return 变更的字段 JSON（空字符串表示没变更），调用方可直接用这个通知 mediad
     */
    std::string handleAttributes(const std::string& attributesJson);

    /** 读取当前完整配置 JSON 字符串 */
    std::string getConfigJson() const;

    /** 读取配置文件路径 */
    const std::string& configPath() const { return m_configPath; }

private:
    /** 把当前配置存到本地文件（原子写：先写 .tmp 再 rename） */
    bool saveToFile() const;

    /** 从本地文件加载配置 */
    bool loadFromFile();

    /** 用默认配置初始化（文件不存在或损坏时兆底） */
    // void applyDefaults();  // 已删除：不再写默认值，等联网后从 TB 同步

    mutable std::mutex m_mutex;
    std::string m_configPath;
    std::string m_configJson;     /* 当前完整配置 JSON 字符串 */
    bool m_loaded = false;
};

} // namespace iot_agent
