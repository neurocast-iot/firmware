/**
 * @file test_string_utils.cpp
 * @brief nc::common::TrimString + ExtractFileNameFromUrl 单测
 *
 * 覆盖场景：
 *   - TrimString：空串、全空白、首尾空白、无空白、中间有空白
 *   - ExtractFileNameFromUrl：普通 URL、带 query、带 fragment、无路径、空串
 */
#include "nc/common/string_utils.h"

#include <gtest/gtest.h>

using nc::common::TrimString;
using nc::common::ExtractFileNameFromUrl;

// ============================================================================
// TrimString 测试
// ============================================================================

TEST(TrimStringTest, EmptyString) {
    EXPECT_EQ("", TrimString(""));
}

TEST(TrimStringTest, AllWhitespace) {
    EXPECT_EQ("", TrimString("   \t\n\r  "));
}

TEST(TrimStringTest, LeadingAndTrailingSpaces) {
    EXPECT_EQ("hello", TrimString("  hello  "));
}

TEST(TrimStringTest, NoWhitespace) {
    EXPECT_EQ("hello", TrimString("hello"));
}

TEST(TrimStringTest, WhitespaceInMiddle) {
    // 中间的空白不能去掉
    EXPECT_EQ("hello world", TrimString("  hello world  "));
}

TEST(TrimStringTest, TabsAndNewlines) {
    EXPECT_EQ("test", TrimString("\t\n test \r\n"));
}

TEST(TrimStringTest, OnlyLeadingSpaces) {
    EXPECT_EQ("hello", TrimString("   hello"));
}

TEST(TrimStringTest, OnlyTrailingSpaces) {
    EXPECT_EQ("hello", TrimString("hello   "));
}

TEST(TrimStringTest, SingleCharacter) {
    EXPECT_EQ("a", TrimString("  a  "));
}

TEST(TrimStringTest, MultipleSpacesInMiddle) {
    EXPECT_EQ("a   b", TrimString("  a   b  "));
}

// ============================================================================
// ExtractFileNameFromUrl 测试
// ============================================================================

TEST(ExtractFileNameFromUrlTest, NormalUrl) {
    EXPECT_EQ("file.tar.gz", ExtractFileNameFromUrl("http://example.com/path/file.tar.gz"));
}

TEST(ExtractFileNameFromUrlTest, WithQuery) {
    // 带 ?query 的 URL，先去掉 query 再提取文件名
    EXPECT_EQ("file.tar.gz", ExtractFileNameFromUrl("http://example.com/path/file.tar.gz?token=abc"));
}

TEST(ExtractFileNameFromUrlTest, WithFragment) {
    // 带 #fragment 的 URL，先去掉 fragment 再提取文件名
    EXPECT_EQ("file.tar.gz", ExtractFileNameFromUrl("http://example.com/path/file.tar.gz#section"));
}

TEST(ExtractFileNameFromUrlTest, WithQueryAndFragment) {
    // 同时带 query 和 fragment
    EXPECT_EQ("file.tar.gz", ExtractFileNameFromUrl("http://example.com/path/file.tar.gz?token=abc#section"));
}

TEST(ExtractFileNameFromUrlTest, NoPath) {
    // 只有域名，没有路径
    EXPECT_EQ("", ExtractFileNameFromUrl("http://example.com/"));
}

TEST(ExtractFileNameFromUrlTest, EmptyString) {
    EXPECT_EQ("", ExtractFileNameFromUrl(""));
}

TEST(ExtractFileNameFromUrlTest, JustFileName) {
    // 只有文件名，没有路径
    EXPECT_EQ("file.txt", ExtractFileNameFromUrl("file.txt"));
}

TEST(ExtractFileNameFromUrlTest, MultipleSlashes) {
    // 多个斜杠
    EXPECT_EQ("file.txt", ExtractFileNameFromUrl("http://example.com///path///file.txt"));
}

TEST(ExtractFileNameFromUrlTest, QueryOnly) {
    // 只有 query，没有路径
    EXPECT_EQ("", ExtractFileNameFromUrl("http://example.com/?token=abc"));
}

TEST(ExtractFileNameFromUrlTest, ComplexFilename) {
    // 复杂文件名（带多个点）
    EXPECT_EQ("my.file.name.tar.gz", ExtractFileNameFromUrl("http://example.com/path/my.file.name.tar.gz"));
}
