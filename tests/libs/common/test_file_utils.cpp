/**
 * @file test_file_utils.cpp
 * @brief nc::common 文件工具单测
 *
 * 覆盖场景：
 *   - FileExists：存在、不存在
 *   - ReadFile：正常读取、文件不存在
 *   - WriteFileAtomic：正常写入、覆盖写入
 *   - FileSize：正常大小、文件不存在
 *   - MakeDirs：正常创建、已存在
 *   - CopyFile：正常复制、源文件不存在
 *   - ExtractFileName：普通路径、无路径、空串
 *
 * 用临时目录做文件操作测试，测试完清理。
 */
#include "nc/common/file_utils.h"

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

using nc::common::FileExists;
using nc::common::ReadFile;
using nc::common::WriteFileAtomic;
using nc::common::FileSize;
using nc::common::MakeDirs;
using nc::common::CopyFile;
using nc::common::ExtractFileName;

// 测试用的临时目录（每个测试用例自己创建、自己清理）
static const char* TEST_DIR = "/tmp/nc_common_file_utils_test";

class FileUtilsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 清理并重建测试目录
        if (system("rm -rf /tmp/nc_common_file_utils_test") != 0) {
            // 忽略清理失败（目录可能不存在）
        }
        ASSERT_TRUE(MakeDirs(TEST_DIR));
    }

    void TearDown() override {
        // 清理测试目录
        if (system("rm -rf /tmp/nc_common_file_utils_test") != 0) {
            // 忽略清理失败
        }
    }

    std::string testPath(const std::string& filename) {
        return std::string(TEST_DIR) + "/" + filename;
    }
};

// ============================================================================
// FileExists 测试
// ============================================================================

TEST_F(FileUtilsTest, FileExists_Exists) {
    const std::string path = testPath("exists.txt");
    ASSERT_TRUE(WriteFileAtomic(path, "hello"));
    EXPECT_TRUE(FileExists(path));
}

TEST_F(FileUtilsTest, FileExists_NotExists) {
    EXPECT_FALSE(FileExists(testPath("not_exists.txt")));
}

TEST_F(FileUtilsTest, FileExists_Directory) {
    // 目录也算"存在"
    const std::string dir = testPath("subdir");
    ASSERT_TRUE(MakeDirs(dir));
    EXPECT_TRUE(FileExists(dir));
}

// ============================================================================
// ReadFile 测试
// ============================================================================

TEST_F(FileUtilsTest, ReadFile_Normal) {
    const std::string path = testPath("read.txt");
    ASSERT_TRUE(WriteFileAtomic(path, "hello world"));

    std::string content;
    ASSERT_TRUE(ReadFile(path, content));
    EXPECT_EQ("hello world", content);
}

TEST_F(FileUtilsTest, ReadFile_EmptyFile) {
    const std::string path = testPath("empty.txt");
    ASSERT_TRUE(WriteFileAtomic(path, ""));

    std::string content;
    ASSERT_TRUE(ReadFile(path, content));
    EXPECT_EQ("", content);
}

TEST_F(FileUtilsTest, ReadFile_NotExists) {
    std::string content;
    EXPECT_FALSE(ReadFile(testPath("not_exists.txt"), content));
}

TEST_F(FileUtilsTest, ReadFile_BinaryContent) {
    // 二进制内容（含 \0）
    const std::string path = testPath("binary.bin");
    const std::string binary_data = std::string("abc") + '\0' + std::string("def");
    ASSERT_TRUE(WriteFileAtomic(path, binary_data));

    std::string content;
    ASSERT_TRUE(ReadFile(path, content));
    EXPECT_EQ(binary_data.size(), content.size());
    EXPECT_EQ(binary_data, content);
}

// ============================================================================
// WriteFileAtomic 测试
// ============================================================================

TEST_F(FileUtilsTest, WriteFileAtomic_Normal) {
    const std::string path = testPath("write.txt");
    ASSERT_TRUE(WriteFileAtomic(path, "test content"));

    std::string content;
    ASSERT_TRUE(ReadFile(path, content));
    EXPECT_EQ("test content", content);
}

TEST_F(FileUtilsTest, WriteFileAtomic_Overwrite) {
    const std::string path = testPath("overwrite.txt");
    ASSERT_TRUE(WriteFileAtomic(path, "first"));
    ASSERT_TRUE(WriteFileAtomic(path, "second"));

    std::string content;
    ASSERT_TRUE(ReadFile(path, content));
    EXPECT_EQ("second", content);
}

TEST_F(FileUtilsTest, WriteFileAtomic_EmptyContent) {
    const std::string path = testPath("empty_write.txt");
    ASSERT_TRUE(WriteFileAtomic(path, ""));

    std::string content;
    ASSERT_TRUE(ReadFile(path, content));
    EXPECT_EQ("", content);
}

// ============================================================================
// FileSize 测试
// ============================================================================

TEST_F(FileUtilsTest, FileSize_Normal) {
    const std::string path = testPath("size.txt");
    ASSERT_TRUE(WriteFileAtomic(path, "hello"));
    EXPECT_EQ(5, FileSize(path));
}

TEST_F(FileUtilsTest, FileSize_EmptyFile) {
    const std::string path = testPath("empty_size.txt");
    ASSERT_TRUE(WriteFileAtomic(path, ""));
    EXPECT_EQ(0, FileSize(path));
}

TEST_F(FileUtilsTest, FileSize_NotExists) {
    EXPECT_EQ(-1, FileSize(testPath("not_exists.txt")));
}

TEST_F(FileUtilsTest, FileSize_LargeFile) {
    const std::string path = testPath("large.bin");
    // 写 1MB 文件
    const std::string data(1024 * 1024, 'x');
    ASSERT_TRUE(WriteFileAtomic(path, data));
    EXPECT_EQ(1024 * 1024, FileSize(path));
}

// ============================================================================
// MakeDirs 测试
// ============================================================================

TEST_F(FileUtilsTest, MakeDirs_Normal) {
    const std::string dir = testPath("a/b/c");
    ASSERT_TRUE(MakeDirs(dir));
    EXPECT_TRUE(FileExists(dir));
}

TEST_F(FileUtilsTest, MakeDirs_AlreadyExists) {
    const std::string dir = testPath("existing");
    ASSERT_TRUE(MakeDirs(dir));
    // 已存在的目录再创建一次，应该返回 true
    EXPECT_TRUE(MakeDirs(dir));
}

TEST_F(FileUtilsTest, MakeDirs_SingleLevel) {
    const std::string dir = testPath("single");
    ASSERT_TRUE(MakeDirs(dir));
    EXPECT_TRUE(FileExists(dir));
}

// ============================================================================
// CopyFile 测试
// ============================================================================

TEST_F(FileUtilsTest, CopyFile_Normal) {
    const std::string src = testPath("src.txt");
    const std::string dst = testPath("dst.txt");
    ASSERT_TRUE(WriteFileAtomic(src, "copy me"));

    ASSERT_TRUE(CopyFile(src, dst));

    std::string content;
    ASSERT_TRUE(ReadFile(dst, content));
    EXPECT_EQ("copy me", content);
}

TEST_F(FileUtilsTest, CopyFile_OverwriteDst) {
    const std::string src = testPath("src2.txt");
    const std::string dst = testPath("dst2.txt");
    ASSERT_TRUE(WriteFileAtomic(src, "new content"));
    ASSERT_TRUE(WriteFileAtomic(dst, "old content"));

    ASSERT_TRUE(CopyFile(src, dst));

    std::string content;
    ASSERT_TRUE(ReadFile(dst, content));
    EXPECT_EQ("new content", content);
}

TEST_F(FileUtilsTest, CopyFile_SrcNotExists) {
    const std::string src = testPath("not_exists_src.txt");
    const std::string dst = testPath("dst3.txt");
    EXPECT_FALSE(CopyFile(src, dst));
}

TEST_F(FileUtilsTest, CopyFile_BinaryContent) {
    const std::string src = testPath("src_bin.bin");
    const std::string dst = testPath("dst_bin.bin");
    const std::string binary_data = std::string("abc") + '\0' + std::string("def");
    ASSERT_TRUE(WriteFileAtomic(src, binary_data));

    ASSERT_TRUE(CopyFile(src, dst));

    std::string content;
    ASSERT_TRUE(ReadFile(dst, content));
    EXPECT_EQ(binary_data, content);
}

// ============================================================================
// ExtractFileName 测试
// ============================================================================

TEST(ExtractFileNameTest, NormalPath) {
    EXPECT_EQ("xxx.jpg", ExtractFileName("/mnt/photos/xxx.jpg"));
}

TEST(ExtractFileNameTest, NoPath) {
    // 只有文件名，没有路径
    EXPECT_EQ("file.txt", ExtractFileName("file.txt"));
}

TEST(ExtractFileNameTest, EmptyString) {
    EXPECT_EQ("", ExtractFileName(""));
}

TEST(ExtractFileNameTest, TrailingSlash) {
    // 路径末尾有斜杠，返回空
    EXPECT_EQ("", ExtractFileName("/mnt/photos/"));
}

TEST(ExtractFileNameTest, MultipleSlashes) {
    EXPECT_EQ("file.txt", ExtractFileName("/mnt//photos//file.txt"));
}

TEST(ExtractFileNameTest, RootPath) {
    EXPECT_EQ("", ExtractFileName("/"));
}

TEST(ExtractFileNameTest, ComplexFilename) {
    // 复杂文件名（带多个点）
    EXPECT_EQ("my.file.name.tar.gz", ExtractFileName("/path/to/my.file.name.tar.gz"));
}
