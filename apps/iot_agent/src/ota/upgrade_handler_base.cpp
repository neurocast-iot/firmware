#include "upgrade_handler_base.h"

#include "nc/common/crypto_utils.h"
#include "nc/common/file_utils.h"
#include "nc/common/log_utils.h"

#include "verifier/manifest_parser.h"

#include <cstdlib>
#include <unistd.h>

namespace iot_agent {
namespace ota {

namespace {

// 同步内核缓冲到磁盘（升级替换了文件，必须刷盘）
void syncDisk() {
    ::sync();
}

} // namespace

// 共同流程：解压 → 解析 manifest → 备份 → 安装 → 校验 → 失败回滚 → 清理
bool UpgradeHandlerBase::executeUpgrade(const std::string& package_path,
                                        const std::string& staging_dir,
                                        std::string& out_version,
                                        std::string& out_error) {
    const char* prefix = getLogPrefix();
    NC_LOGI("{} ====== 开始升级 ======", prefix);
    NC_LOGI("{} 升级包: {}", prefix, package_path.c_str());

    // 1) 包文件必须存在
    if (!nc::common::FileExists(package_path)) {
        out_error = "升级包不存在: " + package_path;
        NC_LOGE("{} {}", prefix, out_error.c_str());
        return false;
    }

    // 2) 解压
    if (!extractPackage(package_path, staging_dir, out_error)) {
        return false;
    }

    // 3) 解析 manifest
    SwManifest manifest;
    const std::string manifest_path = staging_dir + "/manifest.json";
    if (!ManifestParser::parse(manifest_path, manifest)) {
        out_error = "解析 manifest 失败: " + manifest.error_msg;
        return false;
    }
    out_version = manifest.version;
    NC_LOGI("{} 目标版本: {} 文件数: {}", prefix, manifest.version.c_str(), manifest.files.size());

    // 4) 备份当前版本
    if (!getBackupManager().backupFiles(manifest, manifest.version)) {
        out_error = "备份失败";
        return false;
    }

    // 5) 安装
    if (!installFiles(manifest, staging_dir, out_error)) {
        NC_LOGE("{} 安装失败: {}", prefix, out_error.c_str());
        std::string rb_err;
        if (!rollback(manifest.version, rb_err)) {
            NC_LOGE("{} 回滚也失败: {}", prefix, rb_err.c_str());
        } else {
            NC_LOGI("{} 回滚成功", prefix);
        }
        return false;
    }

    // 6) 校验新文件
    if (!verifyNewFiles(manifest, out_error)) {
        NC_LOGE("{} 校验失败: {}", prefix, out_error.c_str());
        std::string rb_err;
        if (!rollback(manifest.version, rb_err)) {
            NC_LOGE("{} 回滚失败: {}", prefix, rb_err.c_str());
        } else {
            NC_LOGI("{} 回滚成功", prefix);
        }
        return false;
    }

    // 7) 清理（备份保留 N 个版本）
    getBackupManager().cleanupOldBackups(OtaConstants::BACKUP_KEEP_COUNT);
    cleanup(staging_dir, package_path);

    NC_LOGI("{} ====== 升级成功 version={} ======", prefix, manifest.version.c_str());
    return true;
}

bool UpgradeHandlerBase::extractPackage(const std::string& package_path,
                                        const std::string& staging_dir,
                                        std::string& out_error) {
    const char* prefix = getLogPrefix();
    if (!nc::common::MakeDirs(staging_dir)) {
        out_error = "创建 staging 目录失败: " + staging_dir;
        return false;
    }
    NC_LOGI("{} 解压: {} -> {}", prefix, package_path.c_str(), staging_dir.c_str());
    // 用 tar 命令解压（busybox 自带）；输出扔掉避免日志污染
    const std::string cmd = "tar -xzf " + package_path + " -C " + staging_dir + " 2>&1";
    if (::system(cmd.c_str()) != 0) {
        out_error = "解压失败: " + package_path;
        return false;
    }
    return true;
}

// 把 manifest 里每个文件从 staging 拷到目标路径
// 目标路径从 manifest 里读（sw 指向 /usr/bin 下的应用文件，fw 指向 /usr 下的系统文件）
bool UpgradeHandlerBase::installFiles(const SwManifest& manifest,
                                      const std::string& staging_dir,
                                      std::string& out_error) {
    const char* prefix = getLogPrefix();
    int installed = 0;
    for (const auto& file : manifest.files) {
        NC_LOGI("{} 安装: {} (diff_mode={})", prefix, file.path.c_str(), file.diff_mode);

        if (file.diff_mode) {
            out_error = "差分模式暂未实现: " + file.path;
            return false;
        }

        const std::string patch_path = staging_dir + "/" + file.patch;
        if (!nc::common::FileExists(patch_path)) {
            out_error = "补丁文件不存在: " + patch_path;
            return false;
        }
        if (!nc::common::CopyFile(patch_path, file.path)) {
            out_error = "复制文件失败: " + file.path;
            return false;
        }
        ++installed;
    }
    syncDisk();
    NC_LOGI("{} 安装完成 共 {} 个文件", prefix, installed);
    return true;
}

// 安装后逐个算 SHA256 和 manifest 里声明的比对
bool UpgradeHandlerBase::verifyNewFiles(const SwManifest& manifest, std::string& out_error) {
    const char* prefix = getLogPrefix();
    for (const auto& file : manifest.files) {
        if (!nc::common::FileExists(file.path)) {
            out_error = "安装后的文件不存在: " + file.path;
            return false;
        }
        const std::string actual = nc::common::CalculateFileSHA256(file.path);
        if (actual != file.new_sha256) {
            out_error = "SHA256 校验失败: " + file.path +
                        " expected=" + file.new_sha256 +
                        " actual=" + actual;
            return false;
        }
    }
    NC_LOGI("{} 所有文件校验通过", prefix);
    return true;
}

bool UpgradeHandlerBase::rollback(const std::string& version, std::string& out_error) {
    const char* prefix = getLogPrefix();
    NC_LOGW("{} 执行回滚 version={}", prefix, version.c_str());
    if (!getBackupManager().restoreFromBackup(version)) {
        out_error = "回滚失败";
        return false;
    }
    syncDisk();
    return true;
}

void UpgradeHandlerBase::cleanup(const std::string& staging_dir, const std::string& package_path) {
    const char* prefix = getLogPrefix();
    NC_LOGI("{} 清理临时文件", prefix);
    (void)::system(("rm -rf " + staging_dir + "/*").c_str());
    if (nc::common::FileExists(package_path)) {
        (void)::unlink(package_path.c_str());
    }
}

BackupManager& UpgradeHandlerBase::getBackupManager() {
    if (!m_backupManager) {
        m_backupManager = std::make_unique<BackupManager>(getBackupRoot());
    }
    return *m_backupManager;
}

} // namespace ota
} // namespace iot_agent
