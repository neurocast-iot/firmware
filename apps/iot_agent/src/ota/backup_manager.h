/**
 * @file backup_manager.h
 * @brief 升级备份管理器
 *
 * 升级前备份要替换的文件，升级失败时从备份恢复。
 * 备份策略：保留完整目录结构（比如 /usr/lib/libfoo.so → backup/1.0.0/usr/lib/libfoo.so）。
 * 这样恢复时不需要知道原始安装路径，直接从备份目录结构反推。
 *
 * sw 和 fw 都可以用这个类，只是构造时传入不同的备份根目录。
 */
#pragma once

#include "ota_types.h"

#include <string>

namespace iot_agent {
namespace ota {

class BackupManager {
public:
    /**
     * @param backup_root 备份根目录（比如 /mnt/emmc/ota/sw/backup 或 /mnt/emmc/ota/fw/backup）
     */
    explicit BackupManager(const std::string& backup_root);

    /**
     * @brief 升级前备份 manifest 中列出的所有文件
     * @param manifest 升级清单（包含要替换的文件路径列表）
     * @param version 版本号（备份目录名）
     * @return true=备份成功
     *
     * 备份路径：backup_root/<version>/<原始路径去掉开头的 />
     * 比如 /usr/lib/libfoo.so → backup_root/1.0.0/usr/lib/libfoo.so
     * 文件不存在就跳过（首次安装的情况）。
     */
    bool backupFiles(const SwManifest& manifest, const std::string& version);

    /**
     * @brief 从备份恢复文件到原始安装路径
     * @param version 版本号
     * @return true=恢复成功
     *
     * 遍历备份目录，根据目录结构反推原始路径，把文件拷回去。
     * 比如 backup_root/1.0.0/usr/lib/libfoo.so → /usr/lib/libfoo.so
     */
    bool restoreFromBackup(const std::string& version);

    /**
     * @brief 清理旧备份，只保留 keep_count 个最新版本（按版本号字典序）
     */
    void cleanupOldBackups(int keep_count);

private:
    /** 备份目录：backup_root/<version>/ */
    std::string getBackupDir(const std::string& version) const {
        return m_backup_root + "/" + version;
    }

    /** 备份根目录 */
    std::string m_backup_root;
};

} // namespace ota
} // namespace iot_agent
