/**
 * @file test_check_disk_space.cpp
 * @brief nc::http::DownloadPolicy::checkDiskSpace + DownloadPolicy 默认值 单测
 *
 * 思路：拿 mkstemp 创个临时文件当 path 入口，传"绝对够"和"绝对不够"两种值。
 * 测试环境是 x86 host，肯定有几百 MB 空间，所以"绝对不够"用一个超大值。
 */
#include "nc/http/download.h"

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

using nc::http::DownloadPolicy;

TEST(CheckDiskSpaceTest, EnoughSpace) {
    /* 当前目录 1MB 肯定够 */
    char tmpl[] = "/tmp/disk_XXXXXX";
    int fd = ::mkstemp(tmpl);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    ::close(fd);

    EXPECT_TRUE(DownloadPolicy::checkDiskSpace(tmpl, 1024 * 1024));
    ::std::remove(tmpl);
}

TEST(CheckDiskSpaceTest, InsufficientSpace) {
    /* 100PB 肯定不够 */
    char tmpl[] = "/tmp/disk_XXXXXX";
    int fd = ::mkstemp(tmpl);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    ::close(fd);

    const uint64_t impossible = 100ULL * 1024 * 1024 * 1024 * 1024 * 1024;
    EXPECT_FALSE(DownloadPolicy::checkDiskSpace(tmpl, impossible));
    ::std::remove(tmpl);
}

TEST(CheckDiskSpaceTest, NonexistentPath) {
    /* 路径不存在 → statvfs 失败 → 返回 false（不能继续下载） */
    EXPECT_FALSE(DownloadPolicy::checkDiskSpace(
        "/tmp/this_path_definitely_does_not_exist_xyz_12345", 1024));
}

TEST(DownloadPolicyTest, DefaultValues) {
    /* DownloadPolicy 默认值要稳定, 改了会破坏所有依赖默认值的调用方 */
    DownloadPolicy p;
    EXPECT_EQ(3, p.max_retries);
    EXPECT_EQ(5, p.retry_delay_sec);
    EXPECT_EQ(600, p.timeout_sec);
    EXPECT_EQ(256ULL * 1024 * 1024, p.max_file_size);
    EXPECT_TRUE(p.resume_support);
    EXPECT_TRUE(p.disk_check);
}
