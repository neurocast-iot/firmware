/**
 * @file test_backup_manager.cpp
 * @brief 升级备份管理器单测
 *
 * 测什么：
 *   - backupFiles() 把文件拷到备份目录，保留原始路径结构
 *   - restoreFromBackup() 从备份目录恢复文件到原始路径
 *   - cleanupOldBackups() 只保留最近 N 个版本
 *
 * 用临时目录模拟文件操作，不碰真实系统文件。
 */
#include "ota/backup_manager.h"
#include "ota/ota_types.h"

#include "nc/common/file_utils.h"

#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace iot_agent {
namespace ota {
namespace {

/* ---- 工具函数 ---- */

/* 创建临时目录，返回目录路径 */
std::string makeTempDir(const std::string& suffix) {
    std::string tmpl = "/tmp/backup_test_" + suffix + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* result = ::mkdtemp(buf.data());
    if (!result) return "";
    return std::string(result);
}

/* 在指定路径写一个文件（自动创建父目录） */
void createFile(const std::string& path, const std::string& content) {
    /* 确保父目录存在 */
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        nc::common::MakeDirs(path.substr(0, slash));
    }
    nc::common::WriteFileAtomic(path, content);
}

/* 读文件内容 */
std::string readFile(const std::string& path) {
    std::string content;
    nc::common::ReadFile(path, content);
    return content;
}

/* 递归删除目录 */
void removeDir(const std::string& path) {
    (void)::system(("rm -rf " + path).c_str());
}

/* 构造一个简单的 SwManifest */
ota::SwManifest makeManifest(const std::vector<std::string>& paths) {
    ota::SwManifest m;
    m.version = "1.0.0";
    m.valid = true;
    for (const auto& p : paths) {
        ota::SwFileEntry e;
        e.path = p;
        e.new_sha256 = "dummy";
        m.files.push_back(e);
    }
    return m;
}

/* ---- 测试 Fixture ---- */

class BackupManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_root = makeTempDir("root");
        m_backupRoot = m_root + "/backup";
        m_fakeFs = m_root + "/fakefs";  /* 模拟设备的文件系统 */
        nc::common::MakeDirs(m_backupRoot);
        nc::common::MakeDirs(m_fakeFs);
    }

    void TearDown() override {
        removeDir(m_root);
    }

    /* 在模拟文件系统里创建文件 */
    std::string fakePath(const std::string& relPath) {
        return m_fakeFs + relPath;
    }

    std::string m_root;
    std::string m_backupRoot;
    std::string m_fakeFs;
};

/* ---- 测试用例 ---- */

TEST_F(BackupManagerTest, BackupFilesCopiesToBackupDir) {
    /* 创建模拟文件 */
    std::string fileA = fakePath("/usr/lib/libfoo.so");
    std::string fileB = fakePath("/usr/bin/myapp");
    createFile(fileA, "libfoo_content");
    createFile(fileB, "myapp_content");

    /* 构造 manifest */
    auto manifest = makeManifest({fileA, fileB});

    /* 执行备份 */
    BackupManager mgr(m_backupRoot);
    ASSERT_TRUE(mgr.backupFiles(manifest, "1.0.0"));

    /* 验证备份文件存在，且内容正确 */
    /* 备份路径：backup_root/1.0.0/<去掉开头/> */
    std::string backupA = m_backupRoot + "/1.0.0/" + fileA.substr(1);
    std::string backupB = m_backupRoot + "/1.0.0/" + fileB.substr(1);

    EXPECT_EQ(readFile(backupA), "libfoo_content");
    EXPECT_EQ(readFile(backupB), "myapp_content");
}

TEST_F(BackupManagerTest, BackupSkipsNonExistentFiles) {
    /* manifest 里有一个不存在的文件 */
    std::string fileA = fakePath("/usr/lib/libfoo.so");
    createFile(fileA, "libfoo_content");
    std::string fileMissing = fakePath("/usr/lib/libmissing.so");

    auto manifest = makeManifest({fileA, fileMissing});

    BackupManager mgr(m_backupRoot);
    /* 不存在的文件跳过，整体应该成功 */
    EXPECT_TRUE(mgr.backupFiles(manifest, "1.0.0"));
}

TEST_F(BackupManagerTest, RestoreFromBackupCopiesBack) {
    /* 先做备份 */
    std::string fileA = fakePath("/usr/lib/libfoo.so");
    createFile(fileA, "original_content");

    auto manifest = makeManifest({fileA});
    BackupManager mgr(m_backupRoot);
    ASSERT_TRUE(mgr.backupFiles(manifest, "1.0.0"));

    /* 模拟升级"破坏"了原文件 */
    createFile(fileA, "new_content");
    EXPECT_EQ(readFile(fileA), "new_content");

    /* 从备份恢复 */
    ASSERT_TRUE(mgr.restoreFromBackup("1.0.0"));

    /* 验证恢复后的内容 */
    EXPECT_EQ(readFile(fileA), "original_content");
}

TEST_F(BackupManagerTest, RestoreNonExistentBackupFails) {
    BackupManager mgr(m_backupRoot);
    /* 备份目录不存在，恢复应该失败 */
    EXPECT_FALSE(mgr.restoreFromBackup("nonexistent_version"));
}

TEST_F(BackupManagerTest, CleanupOldBackupsKeepsLatest) {
    /* 创建 4 个版本的备份目录 */
    for (const auto& ver : {"1.0.0", "1.1.0", "1.2.0", "2.0.0"}) {
        std::string dir = m_backupRoot + "/" + ver;
        nc::common::MakeDirs(dir);
        /* 放一个文件让目录不为空 */
        createFile(dir + "/marker.txt", ver);
    }

    BackupManager mgr(m_backupRoot);
    /* 只保留 2 个最新版本 */
    mgr.cleanupOldBackups(2);

    /* 验证：旧的被删了，新的还在 */
    EXPECT_FALSE(nc::common::FileExists(m_backupRoot + "/1.0.0"));
    EXPECT_FALSE(nc::common::FileExists(m_backupRoot + "/1.1.0"));
    EXPECT_TRUE(nc::common::FileExists(m_backupRoot + "/1.2.0"));
    EXPECT_TRUE(nc::common::FileExists(m_backupRoot + "/2.0.0"));
}

TEST_F(BackupManagerTest, CleanupDoesNothingWhenUnderLimit) {
    /* 只有 1 个版本，keep_count=2，不应该删 */
    std::string dir = m_backupRoot + "/1.0.0";
    nc::common::MakeDirs(dir);
    createFile(dir + "/marker.txt", "data");

    BackupManager mgr(m_backupRoot);
    mgr.cleanupOldBackups(2);

    EXPECT_TRUE(nc::common::FileExists(dir));
}

TEST_F(BackupManagerTest, BackupMultipleFiles) {
    /* 备份多个文件，验证目录结构保留 */
    std::string f1 = fakePath("/etc/config/app.conf");
    std::string f2 = fakePath("/usr/lib/libbar.so");
    std::string f3 = fakePath("/usr/bin/worker");
    createFile(f1, "config_data");
    createFile(f2, "libbar_data");
    createFile(f3, "worker_data");

    auto manifest = makeManifest({f1, f2, f3});
    BackupManager mgr(m_backupRoot);
    ASSERT_TRUE(mgr.backupFiles(manifest, "2.0.0"));

    /* 验证每个文件都在正确的备份路径 */
    EXPECT_EQ(readFile(m_backupRoot + "/2.0.0/" + f1.substr(1)), "config_data");
    EXPECT_EQ(readFile(m_backupRoot + "/2.0.0/" + f2.substr(1)), "libbar_data");
    EXPECT_EQ(readFile(m_backupRoot + "/2.0.0/" + f3.substr(1)), "worker_data");
}

TEST_F(BackupManagerTest, RestoreMultipleFiles) {
    /* 备份多个文件，修改它们，然后恢复 */
    std::string f1 = fakePath("/etc/config/app.conf");
    std::string f2 = fakePath("/usr/lib/libbar.so");
    createFile(f1, "original_config");
    createFile(f2, "original_lib");

    auto manifest = makeManifest({f1, f2});
    BackupManager mgr(m_backupRoot);
    ASSERT_TRUE(mgr.backupFiles(manifest, "1.0.0"));

    /* 模拟升级修改了文件 */
    createFile(f1, "new_config");
    createFile(f2, "new_lib");

    /* 恢复 */
    ASSERT_TRUE(mgr.restoreFromBackup("1.0.0"));

    EXPECT_EQ(readFile(f1), "original_config");
    EXPECT_EQ(readFile(f2), "original_lib");
}

} // namespace
} // namespace ota
} // namespace iot_agent
