/**
 * @file ota_types.h
 * @brief OTA 模块共享类型（manifest 文件描述、备份清单、进度信息等）
 *
 * 这些类型在 SwHandler / SwBackup / ManifestParser / OtaDownloader 之间流转。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iot_agent {

/**
 * OTA 升级包类型
 * - FIRMWARE：固件升级（替换 /usr 下的系统文件）
 * - SOFTWARE：软件升级（替换应用文件）
 */
enum class OtaPackageType {
    FIRMWARE = 0,
    SOFTWARE = 1
};

/** 升级类型字符串前缀（用于状态上报字段名） */
inline const char* otaPrefix(OtaPackageType type) {
    return (type == OtaPackageType::SOFTWARE) ? "sw" : "fw";
}

/** 升级类型友好名（用于日志打印） */
inline const char* otaTypeName(OtaPackageType type) {
    return (type == OtaPackageType::SOFTWARE) ? "software" : "firmware";
}

/** string "fw"/"sw" 转枚举（默认按 FIRMWARE 处理） */
inline OtaPackageType parseOtaType(const std::string& s) {
    return (s == "sw") ? OtaPackageType::SOFTWARE : OtaPackageType::FIRMWARE;
}

/**
 * OTA 升级来源
 * - REMOTE：云端推送（MQTT 通知）
 * - LOCAL：本地升级（SCP 上传到设备的升级包）
 */
enum class OtaSource {
    REMOTE = 0,
    LOCAL = 1
};

/** 升级来源友好名（用于日志打印） */
inline const char* otaSourceName(OtaSource source) {
    return (source == OtaSource::LOCAL) ? "local" : "remote";
}

/**
 * OTA 升级通知（云端属性里的 fw_* / sw_* 字段解析出来的结果）
 *
 * 由平台适配器（TBAdapter::parseMessage）生成，CloudService 转成这个结构体
 * 回调给上层，OtaManager 消费。
 *
 * 为什么定义在 iot_agent 命名空间而不是 ota 命名空间：
 *   它是"云平台层 → OTA 模块"的交接数据，CloudService（不在 ota 命名空间）要用。
 *   定义在这里，ota_manager.h 就不用去 include router/config_router.h 了——
 *   OTA 模块不应该依赖 router 模块。
 */
struct OtaNotice {
    std::string type;               /* "fw" 或 "sw" */
    std::string title;
    std::string version;
    std::string url;
    std::string checksum;
    std::string checksumAlgorithm;
    int         size = 0;
};

namespace ota {

/** 单个升级文件描述（来自 manifest.json） */
struct SwFileEntry {
    std::string path;        ///< 安装路径（如 /usr/bin/iot_live）
    bool diff_mode = false;  ///< true=差分模式（预留），false=完整文件模式
    std::string old_sha256;  ///< 旧文件 SHA256（差分模式必填，完整模式可空）
    std::string new_sha256;  ///< 新文件 SHA256（用于安装后校验）
    size_t new_size = 0;     ///< 新文件大小（字节）
    std::string patch;       ///< 补丁文件相对路径（如 files/usr/bin/iot_live）
};

/** manifest.json 解析结果 */
struct SwManifest {
    std::string version;                 ///< 目标版本号
    std::string base_dir;                ///< 安装根目录
    int file_count = 0;                  ///< 总文件数
    int diff_count = 0;                  ///< 差分文件数
    int full_count = 0;                  ///< 完整文件数
    std::vector<SwFileEntry> files;      ///< 文件列表
    bool valid = false;                  ///< 解析是否成功
    std::string error_msg;               ///< 错误信息（解析失败时）
};

/** 备份文件描述 */
struct BackupEntry {
    std::string name;             ///< 文件名
    std::string original_path;    ///< 原始安装路径
    std::string backup_path;      ///< 备份路径
    std::string sha256;           ///< 文件 SHA256
    size_t size = 0;              ///< 文件大小
};

/** 备份清单 */
struct BackupManifest {
    std::string backup_version;        ///< 备份版本号
    std::string backup_time;           ///< 备份时间
    std::string source_version;        ///< 源版本号
    std::vector<BackupEntry> files;    ///< 备份文件列表
};

/** 升级阶段 */
enum class OtaStage {
    IDLE,
    DOWNLOADING,
    VERIFYING,
    INSTALLING,
    COMPLETED,
    FAILED
};

/** 升级进度信息（用于下载进度回调） */
struct OtaProgressInfo {
    OtaStage stage = OtaStage::IDLE;       ///< 当前阶段
    int progress_percent = 0;              ///< 进度百分比 (0-100)
    uint64_t downloaded_bytes = 0;         ///< 已下载字节数
    uint64_t total_bytes = 0;              ///< 总字节数
    std::string message;                   ///< 状态消息
};

/** 下载进度回调函数类型 */
using ProgressCallback = std::function<void(const OtaProgressInfo& progress)>;

} // namespace ota
} // namespace iot_agent
