/**
 * @file test_token_store.cpp
 * @brief TB token 本地持久化单测
 *
 * 测什么：
 *   - save + load 往返一致性
 *   - 文件不存在/损坏/空 token 等边界条件
 *   - 自动创建父目录
 *   - 覆盖已有 token
 */
#include "cloud/thingsboard/token_store.h"

#include "nc/common/file_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

namespace iot_agent {
namespace {

/* 生成唯一临时文件路径 */
std::string tempPath(const std::string& suffix) {
    char tmpl[] = "/tmp/token_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd >= 0) ::close(fd);
    return std::string(tmpl) + suffix;
}

/* 清理临时文件 */
void cleanup(const std::string& path) {
    std::remove(path.c_str());
}

/* 写内容到文件 */
void writeFile(const std::string& path, const std::string& content) {
    nc::common::WriteFileAtomic(path, content);
}

/* ---- 测试用例 ---- */

TEST(TokenStoreTest, SaveAndLoadRoundTrip) {
    std::string path = tempPath(".json");
    std::string token = "test_token_abc123";

    ASSERT_TRUE(TokenStore::save(path, token));

    std::string loaded;
    ASSERT_TRUE(TokenStore::load(path, loaded));
    EXPECT_EQ(loaded, token);

    cleanup(path);
}

TEST(TokenStoreTest, LoadNonExistentFile) {
    std::string token;
    EXPECT_FALSE(TokenStore::load("/tmp/nonexistent_token_file_xyz.json", token));
}

TEST(TokenStoreTest, LoadCorruptedJson) {
    std::string path = tempPath(".json");
    writeFile(path, "this is not json {{{");

    std::string token;
    EXPECT_FALSE(TokenStore::load(path, token));

    cleanup(path);
}

TEST(TokenStoreTest, LoadEmptyTokenField) {
    std::string path = tempPath(".json");
    writeFile(path, R"({"token":""})");

    std::string token;
    EXPECT_FALSE(TokenStore::load(path, token));

    cleanup(path);
}

TEST(TokenStoreTest, LoadMissingTokenField) {
    std::string path = tempPath(".json");
    writeFile(path, R"({"timestamp":1234567890})");

    std::string token;
    EXPECT_FALSE(TokenStore::load(path, token));

    cleanup(path);
}

TEST(TokenStoreTest, SaveEmptyToken) {
    std::string path = tempPath(".json");
    EXPECT_FALSE(TokenStore::save(path, ""));
    cleanup(path);
}

TEST(TokenStoreTest, SaveCreatesParentDirs) {
    /* 目录不存在时 save 自动创建父目录 */
    std::string dir = "/tmp/token_test_subdir_" + std::to_string(::getpid());
    std::string path = dir + "/nested/token.json";

    ASSERT_TRUE(TokenStore::save(path, "auto_mkdir_token"));

    std::string loaded;
    ASSERT_TRUE(TokenStore::load(path, loaded));
    EXPECT_EQ(loaded, "auto_mkdir_token");

    cleanup(path);
    /* 清理目录 */
    std::remove(path.c_str());
    std::remove(dir.c_str());
    std::remove((dir + "/nested").c_str());
}

TEST(TokenStoreTest, OverwriteExistingToken) {
    std::string path = tempPath(".json");

    ASSERT_TRUE(TokenStore::save(path, "first_token"));
    ASSERT_TRUE(TokenStore::save(path, "second_token"));

    std::string loaded;
    ASSERT_TRUE(TokenStore::load(path, loaded));
    EXPECT_EQ(loaded, "second_token");

    cleanup(path);
}

} // namespace
} // namespace iot_agent
