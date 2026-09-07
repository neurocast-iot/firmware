/**
 * @file test_manifest_parser.cpp
 * @brief ManifestParser 单测：合法 / 非法 / 缺字段 / 文件不存在
 */
#include "manifest_parser.h"
#include "ota_types.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace iot_agent {
namespace ota {
namespace {

/* 工具：把字符串写到临时文件，返回路径 */
std::string writeTempFile(const std::string& content) {
    /* mkstemp 安全生成唯一文件 */
    char tmpl[] = "/tmp/manifest_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) return "";
    ssize_t n = ::write(fd, content.data(), content.size());
    static_cast<void>(n);
    ::close(fd);
    return tmpl;
}

const char* kValidManifest = R"({
    "version": "1.2.3",
    "base_dir": "/",
    "files": [
        {
            "path": "/usr/bin/iot_agent",
            "diff_mode": false,
            "new_sha256": "abc123def456",
            "new_size": 1234567,
            "patch": "files/usr/bin/iot_agent"
        },
        {
            "path": "/etc/config/iot_agent.json",
            "diff_mode": true,
            "old_sha256": "oldold",
            "new_sha256": "newnew",
            "new_size": 1024,
            "patch": "files/etc/config/iot_agent.json"
        }
    ]
})";

TEST(ManifestParserTest, ValidManifest) {
    auto path = writeTempFile(kValidManifest);
    ASSERT_FALSE(path.empty());

    SwManifest m;
    EXPECT_TRUE(ManifestParser::parse(path, m));
    EXPECT_TRUE(m.valid);
    EXPECT_EQ("1.2.3", m.version);
    EXPECT_EQ("/", m.base_dir);
    EXPECT_EQ(2, m.file_count);
    EXPECT_EQ(1, m.diff_count);
    EXPECT_EQ(1, m.full_count);
    ASSERT_EQ(2u, m.files.size());

    EXPECT_EQ("/usr/bin/iot_agent", m.files[0].path);
    EXPECT_FALSE(m.files[0].diff_mode);
    EXPECT_EQ("abc123def456", m.files[0].new_sha256);
    EXPECT_EQ(1234567u, m.files[0].new_size);

    EXPECT_TRUE(m.files[1].diff_mode);
    EXPECT_EQ("oldold", m.files[1].old_sha256);

    ::std::remove(path.c_str());
}

TEST(ManifestParserTest, MissingFile) {
    SwManifest m;
    EXPECT_FALSE(ManifestParser::parse("/tmp/nonexistent_xyz_12345.json", m));
    EXPECT_FALSE(m.valid);
    EXPECT_FALSE(m.error_msg.empty());
}

TEST(ManifestParserTest, InvalidJson) {
    auto path = writeTempFile("{ this is not valid json");
    SwManifest m;
    EXPECT_FALSE(ManifestParser::parse(path, m));
    EXPECT_FALSE(m.valid);
    ::std::remove(path.c_str());
}

TEST(ManifestParserTest, MissingFilesArray) {
    /* 没有 files 数组：解析应该失败（files 必填） */
    auto path = writeTempFile(R"({"version": "1.0", "base_dir": "/"})");
    SwManifest m;
    EXPECT_FALSE(ManifestParser::parse(path, m));
    EXPECT_FALSE(m.valid);
    ::std::remove(path.c_str());
}

TEST(ManifestParserTest, EmptyFilesArray) {
    /* files 是空数组：合法，但 file_count=0 */
    auto path = writeTempFile(R"({"version": "1.0", "base_dir": "/", "files": []})");
    SwManifest m;
    EXPECT_TRUE(ManifestParser::parse(path, m));
    EXPECT_TRUE(m.valid);
    EXPECT_EQ(0, m.file_count);
    EXPECT_TRUE(m.files.empty());
    ::std::remove(path.c_str());
}

}  // namespace
}  // namespace ota
}  // namespace iot_agent
