/**
 * @file test_ota_config.cpp
 * @brief OTA 路径与常量单测
 *
 * 测什么：
 *   - OtaPaths 的路径拼接函数（swInbox/fwStaging 等）
 *   - OtaConstants 的常量值是否符合预期
 *
 * 这些全是 static inline / constexpr，不需要拉 .cpp 重编译。
 */
#include "ota/ota_config.h"

#include <gtest/gtest.h>

namespace iot_agent {
namespace ota {
namespace {

/* ---- OtaPaths 路径拼接 ---- */

TEST(OtaPathsTest, SwInbox) {
    EXPECT_EQ(OtaPaths::swInbox("/mnt/emmc/ota"), "/mnt/emmc/ota/sw/inbox");
}

TEST(OtaPathsTest, SwDownload) {
    EXPECT_EQ(OtaPaths::swDownload("/data/ota"), "/data/ota/sw/download");
}

TEST(OtaPathsTest, SwStaging) {
    EXPECT_EQ(OtaPaths::swStaging("/data/ota"), "/data/ota/sw/staging");
}

TEST(OtaPathsTest, SwBackup) {
    EXPECT_EQ(OtaPaths::swBackup("/data/ota"), "/data/ota/sw/backup");
}

TEST(OtaPathsTest, FwInbox) {
    EXPECT_EQ(OtaPaths::fwInbox("/mnt/emmc/ota"), "/mnt/emmc/ota/fw/inbox");
}

TEST(OtaPathsTest, FwDownload) {
    EXPECT_EQ(OtaPaths::fwDownload("/data/ota"), "/data/ota/fw/download");
}

TEST(OtaPathsTest, FwStaging) {
    EXPECT_EQ(OtaPaths::fwStaging("/data/ota"), "/data/ota/fw/staging");
}

TEST(OtaPathsTest, FwBackup) {
    EXPECT_EQ(OtaPaths::fwBackup("/data/ota"), "/data/ota/fw/backup");
}

TEST(OtaPathsTest, CustomBaseDir) {
    /* 不同的 base_dir 应该正确拼接 */
    EXPECT_EQ(OtaPaths::swInbox("/custom/path"), "/custom/path/sw/inbox");
    EXPECT_EQ(OtaPaths::fwStaging("/tmp/test"), "/tmp/test/fw/staging");
}

/* ---- OtaPaths 常量（版本文件路径固定，不随 base_dir 变） ---- */

TEST(OtaPathsTest, VersionFilePaths) {
    EXPECT_STREQ(OtaPaths::CURRENT_SW_VERSION, "/etc/config/ota/current_sw_version");
    EXPECT_STREQ(OtaPaths::CURRENT_FW_VERSION, "/etc/config/ota/current_fw_version");
}

TEST(OtaPathsTest, DefaultBase) {
    EXPECT_STREQ(OtaPaths::OTA_BASE, "/mnt/emmc/ota");
}

/* ---- OtaConstants 常量值 ---- */

TEST(OtaConstantsTest, DownloadParams) {
    EXPECT_EQ(OtaConstants::DOWNLOAD_TIMEOUT_SEC, 600);
    EXPECT_EQ(OtaConstants::MAX_RETRY, 3);
    EXPECT_EQ(OtaConstants::RETRY_DELAY_SEC, 5);
    EXPECT_EQ(OtaConstants::MAX_FILE_SIZE, 256 * 1024 * 1024);
}

TEST(OtaConstantsTest, BackupKeepCount) {
    EXPECT_EQ(OtaConstants::BACKUP_KEEP_COUNT, 2);
}

TEST(OtaConstantsTest, RebootDelay) {
    EXPECT_EQ(OtaConstants::REBOOT_DELAY_SEC, 10);
}

} // namespace
} // namespace ota
} // namespace iot_agent
