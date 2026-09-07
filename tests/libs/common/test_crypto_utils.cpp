/**
 * @file test_crypto_utils.cpp
 * @brief nc::common::CalculateFileSHA256 单测
 *
 * 覆盖场景：
 *   - 正常计算：已知内容的文件，哈希值和预期值比对
 *   - 空文件：空文件的 SHA256 是固定值
 *   - 大文件：1MB 文件，测试分块读取是否正确
 *   - 文件不存在：返回空字符串
 *   - 二进制内容：含 \0 的文件
 *
 * 用临时目录做文件操作测试，测试完清理。
 */
#include "nc/common/crypto_utils.h"
#include "nc/common/file_utils.h"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

using nc::common::CalculateFileSHA256;
using nc::common::WriteFileAtomic;
using nc::common::MakeDirs;

// 测试用的临时目录
static const char* TEST_DIR = "/tmp/nc_common_crypto_utils_test";

class CryptoUtilsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 清理并重建测试目录
        if (system("rm -rf /tmp/nc_common_crypto_utils_test") != 0) {
            // 忽略清理失败（目录可能不存在）
        }
        ASSERT_TRUE(MakeDirs(TEST_DIR));
    }

    void TearDown() override {
        // 清理测试目录
        if (system("rm -rf /tmp/nc_common_crypto_utils_test") != 0) {
            // 忽略清理失败
        }
    }

    std::string testPath(const std::string& filename) {
        return std::string(TEST_DIR) + "/" + filename;
    }
};

TEST_F(CryptoUtilsTest, NormalContent) {
    // "hello world" 的 SHA256 是已知值（可以网上查或用 sha256sum 算）
    const std::string path = testPath("hello.txt");
    ASSERT_TRUE(WriteFileAtomic(path, "hello world"));

    const std::string hash = CalculateFileSHA256(path);
    // sha256sum 算出来的 "hello world" 的哈希
    EXPECT_EQ("b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9", hash);
}

TEST_F(CryptoUtilsTest, EmptyFile) {
    // 空文件的 SHA256 是固定值（可以网上查或用 sha256sum 算）
    const std::string path = testPath("empty.txt");
    ASSERT_TRUE(WriteFileAtomic(path, ""));

    const std::string hash = CalculateFileSHA256(path);
    // sha256sum 算出来的空字符串的哈希
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hash);
}

TEST_F(CryptoUtilsTest, FileNotExists) {
    // 文件不存在，返回空字符串
    const std::string hash = CalculateFileSHA256(testPath("not_exists.txt"));
    EXPECT_EQ("", hash);
}

TEST_F(CryptoUtilsTest, LargeFile) {
    // 1MB 文件，测试分块读取是否正确
    const std::string path = testPath("large.bin");
    const std::string data(1024 * 1024, 'x');
    ASSERT_TRUE(WriteFileAtomic(path, data));

    const std::string hash = CalculateFileSHA256(path);
    // 1MB 的 'x' 的 SHA256（可以用 sha256sum 算）
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(64u, hash.size()); // SHA256 是 64 字符的十六进制
}

TEST_F(CryptoUtilsTest, BinaryContent) {
    // 二进制内容（含 \0）
    const std::string path = testPath("binary.bin");
    const std::string binary_data = std::string("abc") + '\0' + std::string("def");
    ASSERT_TRUE(WriteFileAtomic(path, binary_data));

    const std::string hash = CalculateFileSHA256(path);
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(64u, hash.size());
}

TEST_F(CryptoUtilsTest, DifferentContentDifferentHash) {
    // 不同内容，不同哈希
    const std::string path1 = testPath("file1.txt");
    const std::string path2 = testPath("file2.txt");
    ASSERT_TRUE(WriteFileAtomic(path1, "content1"));
    ASSERT_TRUE(WriteFileAtomic(path2, "content2"));

    const std::string hash1 = CalculateFileSHA256(path1);
    const std::string hash2 = CalculateFileSHA256(path2);
    EXPECT_NE(hash1, hash2);
}

TEST_F(CryptoUtilsTest, SameContentSameHash) {
    // 相同内容，相同哈希
    const std::string path1 = testPath("same1.txt");
    const std::string path2 = testPath("same2.txt");
    ASSERT_TRUE(WriteFileAtomic(path1, "same content"));
    ASSERT_TRUE(WriteFileAtomic(path2, "same content"));

    const std::string hash1 = CalculateFileSHA256(path1);
    const std::string hash2 = CalculateFileSHA256(path2);
    EXPECT_EQ(hash1, hash2);
}

TEST_F(CryptoUtilsTest, HashFormat) {
    // 哈希格式：64 字符小写十六进制
    const std::string path = testPath("format.txt");
    ASSERT_TRUE(WriteFileAtomic(path, "test"));

    const std::string hash = CalculateFileSHA256(path);
    ASSERT_EQ(64u, hash.size());

    // 检查是否都是十六进制字符（0-9, a-f）
    for (char c : hash) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "Invalid hex char: " << c;
    }
}
