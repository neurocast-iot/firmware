/**
 * @file test_ota_types.cpp
 * @brief OTA 类型工具函数单测：parseOtaType / otaPrefix / otaTypeName / otaSourceName
 *
 * 这些函数都是 inline 定义在 ota_types.h 里，不需要拉 .cpp 重编译。
 */
#include "ota/ota_types.h"

#include <gtest/gtest.h>

namespace iot_agent {
namespace {

/* ---- parseOtaType ---- */

TEST(OtaTypesTest, ParseOtaTypeFw) {
    EXPECT_EQ(parseOtaType("fw"), OtaPackageType::FIRMWARE);
}

TEST(OtaTypesTest, ParseOtaTypeSw) {
    EXPECT_EQ(parseOtaType("sw"), OtaPackageType::SOFTWARE);
}

TEST(OtaTypesTest, ParseOtaTypeUnknown) {
    /* 未知字符串默认按 FIRMWARE 处理 */
    EXPECT_EQ(parseOtaType("unknown"), OtaPackageType::FIRMWARE);
}

TEST(OtaTypesTest, ParseOtaTypeEmpty) {
    /* 空字符串也按 FIRMWARE 处理 */
    EXPECT_EQ(parseOtaType(""), OtaPackageType::FIRMWARE);
}

/* ---- otaPrefix ---- */

TEST(OtaTypesTest, OtaPrefixFw) {
    EXPECT_STREQ(otaPrefix(OtaPackageType::FIRMWARE), "fw");
}

TEST(OtaTypesTest, OtaPrefixSw) {
    EXPECT_STREQ(otaPrefix(OtaPackageType::SOFTWARE), "sw");
}

/* ---- otaTypeName ---- */

TEST(OtaTypesTest, OtaTypeNameFw) {
    EXPECT_STREQ(otaTypeName(OtaPackageType::FIRMWARE), "firmware");
}

TEST(OtaTypesTest, OtaTypeNameSw) {
    EXPECT_STREQ(otaTypeName(OtaPackageType::SOFTWARE), "software");
}

/* ---- otaSourceName ---- */

TEST(OtaTypesTest, OtaSourceNameRemote) {
    EXPECT_STREQ(otaSourceName(OtaSource::REMOTE), "remote");
}

TEST(OtaTypesTest, OtaSourceNameLocal) {
    EXPECT_STREQ(otaSourceName(OtaSource::LOCAL), "local");
}

} // namespace
} // namespace iot_agent
