/**
 * @file ota_config.h
 * @brief OTA 路径与常量配置
 *
 * 路径规则：
 *   - OTA 基础目录从配置文件读（默认 /mnt/emmc/ota），运行时可改
 *   - sw/ 子目录放软件升级相关；fw/ 子目录放固件升级相关
 *   - inbox 目录用于本地升级（SCP 上传升级包到这里）
 *   - staging 目录用于解压和处理升级包
 *   - backup 目录用于升级前备份，失败时回滚
 *   - /etc/config/ota/ 记录当前安装的版本号
 *
 * 使用方式：
 *   - OtaPaths::swInbox(base_dir) 返回完整路径（base_dir + "/sw/inbox"）
 *   - OtaPaths::OTA_BASE 是默认值，配置文件没指定时用
 */
#pragma once

#include <string>

namespace iot_agent {
namespace ota {

struct OtaPaths {
    // OTA 基础目录默认值（配置文件没指定时用）
    static constexpr const char* OTA_BASE = "/mnt/emmc/ota";

    // 相对路径常量（不包含基础目录）
    static constexpr const char* SW_INBOX_REL     = "/sw/inbox";
    static constexpr const char* SW_DOWNLOAD_REL  = "/sw/download";
    static constexpr const char* SW_STAGING_REL   = "/sw/staging";
    static constexpr const char* SW_BACKUP_REL    = "/sw/backup";
    static constexpr const char* FW_INBOX_REL     = "/fw/inbox";
    static constexpr const char* FW_DOWNLOAD_REL  = "/fw/download";
    static constexpr const char* FW_STAGING_REL   = "/fw/staging";
    static constexpr const char* FW_BACKUP_REL    = "/fw/backup";

    // 版本记录文件（固定，不随 base_dir 变）
    static constexpr const char* CURRENT_SW_VERSION = "/etc/config/ota/current_sw_version";
    static constexpr const char* CURRENT_FW_VERSION = "/etc/config/ota/current_fw_version";

    // 拼接完整路径的函数（base_dir + 相对路径）
    static std::string swInbox(const std::string& base)     { return base + SW_INBOX_REL; }
    static std::string swDownload(const std::string& base)  { return base + SW_DOWNLOAD_REL; }
    static std::string swStaging(const std::string& base)   { return base + SW_STAGING_REL; }
    static std::string swBackup(const std::string& base)    { return base + SW_BACKUP_REL; }
    static std::string fwInbox(const std::string& base)     { return base + FW_INBOX_REL; }
    static std::string fwDownload(const std::string& base)  { return base + FW_DOWNLOAD_REL; }
    static std::string fwStaging(const std::string& base)   { return base + FW_STAGING_REL; }
    static std::string fwBackup(const std::string& base)    { return base + FW_BACKUP_REL; }
};

struct OtaConstants {
    // 下载参数
    static constexpr int DOWNLOAD_TIMEOUT_SEC = 600;     // 下载超时（秒）
    static constexpr int MAX_RETRY = 3;                  // 最大重试次数
    static constexpr int RETRY_DELAY_SEC = 5;            // 重试延迟（秒）
    static constexpr int MAX_FILE_SIZE = 256 * 1024 * 1024;  // 最大文件大小（256MB）

    // 备份管理
    static constexpr int BACKUP_KEEP_COUNT = 2;          // 保留最近几个版本的备份

    // 升级后行为
    static constexpr int REBOOT_DELAY_SEC = 10;          // 升级成功后延迟多久重启（秒）
};

} // namespace ota
} // namespace iot_agent
