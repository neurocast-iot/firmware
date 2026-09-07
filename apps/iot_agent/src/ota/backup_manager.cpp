#include "backup_manager.h"

#include "nc/common/file_utils.h"
#include "nc/common/log_utils.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace iot_agent {
namespace ota {

namespace {

// 同步内核缓冲到磁盘（升级替换了文件，必须刷盘）
void syncDisk() {
    ::sync();
}

// 取路径的目录部分（比如 "/usr/lib/libfoo.so" → "/usr/lib"）
std::string dirName(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

} // namespace

BackupManager::BackupManager(const std::string& backup_root)
    : m_backup_root(backup_root) {
}

// 升级前：把要替换的文件原样拷贝到 backup/<version>/<原始路径去掉开头的 />
// 保留完整目录结构，恢复时不需要知道原始安装路径
bool BackupManager::backupFiles(const SwManifest& manifest, const std::string& version) {
    const std::string backup_dir = getBackupDir(version);
    if (!nc::common::MakeDirs(backup_dir)) {
        NC_LOGE("[Backup] 创建备份目录失败: {}", backup_dir.c_str());
        return false;
    }

    int backed = 0;
    for (const auto& file : manifest.files) {
        if (!nc::common::FileExists(file.path)) {
            NC_LOGI("[Backup] 文件不存在，跳过备份 path={}", file.path.c_str());
            continue;
        }

        // 备份路径：backup/<version>/<原始路径去掉开头的 />
        // 比如 /usr/lib/libfoo.so → backup/1.0.0/usr/lib/libfoo.so
        std::string rel_path = file.path;
        if (!rel_path.empty() && rel_path[0] == '/') {
            rel_path = rel_path.substr(1);
        }
        const std::string backup_path = backup_dir + "/" + rel_path;
        if (!nc::common::MakeDirs(dirName(backup_path))) {
            NC_LOGE("[Backup] 创建备份子目录失败: {}", backup_path.c_str());
            return false;
        }
        if (!nc::common::CopyFile(file.path, backup_path)) {
            NC_LOGE("[Backup] 备份文件失败: {} -> {}", file.path.c_str(), backup_path.c_str());
            return false;
        }
        ++backed;
    }

    syncDisk();
    NC_LOGI("[Backup] 备份完成 version={} files={}", version.c_str(), backed);
    return true;
}

// 升级失败时：从 backup/<version>/ 把文件拷回原始路径
// 遍历备份目录，根据目录结构反推原始路径
bool BackupManager::restoreFromBackup(const std::string& version) {
    const std::string backup_dir = getBackupDir(version);
    if (!nc::common::FileExists(backup_dir)) {
        NC_LOGE("[Backup] 备份目录不存在: {}", backup_dir.c_str());
        return false;
    }

    // 用 find 命令遍历备份目录（递归），找所有普通文件
    // 输出格式：./usr/lib/libfoo.so
    std::string cmd = "cd " + backup_dir + " && find . -type f";
    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) {
        NC_LOGE("[Backup] 执行 find 命令失败");
        return false;
    }

    char line[1024];
    int restored = 0;
    while (::fgets(line, sizeof(line), fp)) {
        // 去掉换行符
        size_t len = ::strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        // line 是 "./usr/lib/libfoo.so" 这种格式
        std::string rel_path = line;
        if (rel_path.size() > 2 && rel_path[0] == '.' && rel_path[1] == '/') {
            rel_path = rel_path.substr(2);
        }
        const std::string backup_path = backup_dir + "/" + rel_path;
        const std::string target_path = "/" + rel_path;
        if (!nc::common::MakeDirs(dirName(target_path))) {
            NC_LOGE("[Backup] 创建恢复目录失败: {}", target_path.c_str());
            ::pclose(fp);
            return false;
        }
        if (!nc::common::CopyFile(backup_path, target_path)) {
            NC_LOGE("[Backup] 恢复文件失败: {} -> {}", backup_path.c_str(), target_path.c_str());
            ::pclose(fp);
            return false;
        }
        ++restored;
        NC_LOGI("[Backup] 已恢复 {}", target_path.c_str());
    }
    ::pclose(fp);

    syncDisk();
    NC_LOGW("[Backup] 恢复完成 files={}", restored);
    return true;
}

// 只保留最近 keep_count 个版本的备份，旧的删掉
void BackupManager::cleanupOldBackups(int keep_count) {
    if (!nc::common::FileExists(m_backup_root)) {
        return;
    }

    // 扫描备份根目录，收集所有子目录名（版本号）
    DIR* d = ::opendir(m_backup_root.c_str());
    if (!d) return;

    std::vector<std::string> versions;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        const std::string path = m_backup_root + "/" + ent->d_name;
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            versions.emplace_back(ent->d_name);
        }
    }
    ::closedir(d);

    if (static_cast<int>(versions.size()) <= keep_count) return;

    // 按版本号字符串排序，删掉最旧的
    std::sort(versions.begin(), versions.end());
    while (static_cast<int>(versions.size()) > keep_count) {
        const std::string old = m_backup_root + "/" + versions.front();
        NC_LOGI("[Backup] 删除旧备份 {}", versions.front().c_str());
        (void)::system(("rm -rf " + old).c_str());
        versions.erase(versions.begin());
    }
}

} // namespace ota
} // namespace iot_agent
