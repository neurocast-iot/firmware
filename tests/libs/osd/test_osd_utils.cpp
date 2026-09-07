/**
 * @file test_osd_utils.cpp
 * @brief osd:: 工具函数单元测试
 *
 * 测什么:
 *   - matchColorIndex: 颜色匹配算法（精确匹配 + 最邻近匹配）
 *   - calculateFontSize: 视频宽度 → 字体大小映射
 *   - utf8WidthUnits: UTF-8 字符串宽度计算（ASCII + 中文）
 *   - calculateTextWidth: 文本画布宽度计算
 *   - clampToBounds: 画布边界限制
 *   - calculateStride: 步长对齐（偶数）
 *
 * 不测:
 *   - convertToWideChar（需要查表实现，单独测）
 *   - OsdEngine（硬件依赖）
 *
 * 测试策略:
 *   - 纯函数，无状态，直接断言返回值
 *   - 边界条件全覆盖（0、负数、最大值）
 *   - 中文/ASCII 混合输入
 */
#include "osd/OsdUtils.h"

#include <gtest/gtest.h>

using namespace osd;

/* ====================================================================
 * matchColorIndex 测试
 * ==================================================================== */

TEST(OsdUtilsColorTest, TransparentColor) {
    // Arrange: 透明色（alpha=0）
    Color c{0, 255, 0, 0};  // alpha=0, RGB 随意

    // Act & Assert: 应该返回 0（透明色索引）
    EXPECT_EQ(0, matchColorIndex(c));
}

TEST(OsdUtilsColorTest, ExactMatchWhite) {
    // Arrange: 纯白色
    Color c{255, 255, 255, 255};

    // Act & Assert: 应该精确匹配到 1（白色索引）
    EXPECT_EQ(1, matchColorIndex(c));
}

TEST(OsdUtilsColorTest, ExactMatchBlack) {
    // Arrange: 纯黑色 (alpha=255, r=0, g=0, b=0)
    Color c{255, 0, 0, 0};

    // Act & Assert: 应该精确匹配到 2（黑色索引）
    EXPECT_EQ(2, matchColorIndex(c));
}

TEST(OsdUtilsColorTest, ExactMatchRed) {
    // Arrange: 纯红色 (alpha=255, r=255, g=0, b=0)
    Color c{255, 255, 0, 0};

    // Act & Assert: 应该精确匹配到 3（红色索引）
    EXPECT_EQ(3, matchColorIndex(c));
}

TEST(OsdUtilsColorTest, NearestMatchCloseToRed) {
    // Arrange: 接近红色但不完全相等 (alpha=255, r=250, g=5, b=5)
    Color c{255, 250, 5, 5};

    // Act & Assert: 应该最邻近匹配到红色（索引 3）
    int idx = matchColorIndex(c);
    EXPECT_EQ(3, idx);
}

TEST(OsdUtilsColorTest, NearestMatchCloseToBlue) {
    // Arrange: 接近蓝色 (alpha=255, r=5, g=5, b=250)
    Color c{255, 5, 5, 250};

    // Act & Assert: 应该最邻近匹配到蓝色（索引 5）
    int idx = matchColorIndex(c);
    EXPECT_EQ(5, idx);
}

/* ====================================================================
 * calculateFontSize 测试
 * ==================================================================== */

TEST(OsdUtilsFontSizeTest, VideoWidth2560) {
    EXPECT_EQ(64, calculateFontSize(2560));
    EXPECT_EQ(64, calculateFontSize(3840));  // 4K
    EXPECT_EQ(64, calculateFontSize(10000));
}

TEST(OsdUtilsFontSizeTest, VideoWidth1920) {
    EXPECT_EQ(48, calculateFontSize(1920));
    EXPECT_EQ(48, calculateFontSize(2559));
}

TEST(OsdUtilsFontSizeTest, VideoWidth1024) {
    EXPECT_EQ(32, calculateFontSize(1024));
    EXPECT_EQ(32, calculateFontSize(1919));
}

TEST(OsdUtilsFontSizeTest, VideoWidth960) {
    EXPECT_EQ(24, calculateFontSize(960));
    EXPECT_EQ(24, calculateFontSize(1023));
}

TEST(OsdUtilsFontSizeTest, VideoWidthSmall) {
    EXPECT_EQ(16, calculateFontSize(640));
    EXPECT_EQ(16, calculateFontSize(0));
    EXPECT_EQ(16, calculateFontSize(-100));  // 负数也应该返回 16
}

/* ====================================================================
 * utf8WidthUnits 测试
 * ==================================================================== */

TEST(OsdUtilsWidthTest, EmptyString) {
    EXPECT_EQ(0, utf8WidthUnits(""));
}

TEST(OsdUtilsWidthTest, PureAscii) {
    // Arrange: 纯 ASCII
    EXPECT_EQ(5, utf8WidthUnits("hello"));
    EXPECT_EQ(1, utf8WidthUnits("A"));
    EXPECT_EQ(10, utf8WidthUnits("0123456789"));
}

TEST(OsdUtilsWidthTest, PureChinese) {
    // Arrange: 纯中文（每个中文字算 2 个单位）
    EXPECT_EQ(2, utf8WidthUnits("中"));
    EXPECT_EQ(4, utf8WidthUnits("中文"));
    EXPECT_EQ(8, utf8WidthUnits("测试文本"));  // 4 个中文字 = 8 单位
}

TEST(OsdUtilsWidthTest, MixedAsciiAndChinese) {
    // Arrange: ASCII + 中文混合
    EXPECT_EQ(3, utf8WidthUnits("A中"));  // 1 + 2 = 3
    EXPECT_EQ(9, utf8WidthUnits("hello中文"));  // 5 + 4 = 9
}

TEST(OsdUtilsWidthTest, InvalidUtf8) {
    // Arrange: 非法 UTF-8（孤立续字节）
    std::string invalid = "\x80\x80\x80";  // 3 个孤立续字节

    // Act & Assert: 应该跳过，不计宽
    EXPECT_EQ(0, utf8WidthUnits(invalid));
}

/* ====================================================================
 * calculateTextWidth 测试
 * ==================================================================== */

TEST(OsdUtilsTextWidthTest, PureAscii) {
    // Arrange: "hello" + font_size=32
    // 宽度 = 32/2 * 5 + 4 = 80 + 4 = 84
    EXPECT_EQ(84, calculateTextWidth("hello", 32));
}

TEST(OsdUtilsTextWidthTest, PureChinese) {
    // Arrange: "中文" + font_size=32
    // 宽度 = 32/2 * 4 + 4 = 64 + 4 = 68
    EXPECT_EQ(68, calculateTextWidth("中文", 32));
}

TEST(OsdUtilsTextWidthTest, EmptyString) {
    // Arrange: 空字符串 + font_size=32
    // 宽度 = 32/2 * 0 + 4 = 0 + 4 = 4
    EXPECT_EQ(4, calculateTextWidth("", 32));
}

/* ====================================================================
 * clampToBounds 测试
 * ==================================================================== */

TEST(OsdUtilsClampTest, NormalCase) {
    // Arrange: 正常情况，不越界
    auto bounds = clampToBounds(100, 100, 200, 200, 1920, 1080, 1920, 1080);

    // Assert: 应该不变
    EXPECT_EQ(100, bounds.x);
    EXPECT_EQ(100, bounds.y);
    EXPECT_EQ(200, bounds.width);
    EXPECT_EQ(200, bounds.height);
}

TEST(OsdUtilsClampTest, NegativeCoordinates) {
    // Arrange: 负坐标
    auto bounds = clampToBounds(-50, -30, 200, 200, 1920, 1080, 1920, 1080);

    // Assert: 应该被修正为 0
    EXPECT_EQ(0, bounds.x);
    EXPECT_EQ(0, bounds.y);
}

TEST(OsdUtilsClampTest, ExceedsVideoBounds) {
    // Arrange: 超出视频边界
    auto bounds = clampToBounds(1800, 1000, 200, 200, 1920, 1080, 1920, 1080);

    // Assert: 应该被推回，不超出
    EXPECT_EQ(1720, bounds.x);  // 1920 - 200
    EXPECT_EQ(880, bounds.y);   // 1080 - 200
}

TEST(OsdUtilsClampTest, ExceedsMaxSize) {
    // Arrange: 尺寸超过 SDK 最大值
    auto bounds = clampToBounds(100, 100, 500, 500, 256, 256, 1920, 1080);

    // Assert: 尺寸应该被限制
    EXPECT_EQ(256, bounds.width);
    EXPECT_EQ(256, bounds.height);
}

/* ====================================================================
 * calculateStride 测试
 * ==================================================================== */

TEST(OsdUtilsStrideTest, EvenWidth) {
    EXPECT_EQ(100, calculateStride(100));
    EXPECT_EQ(200, calculateStride(200));
}

TEST(OsdUtilsStrideTest, OddWidth) {
    EXPECT_EQ(102, calculateStride(101));
    EXPECT_EQ(202, calculateStride(201));
}

TEST(OsdUtilsStrideTest, ZeroWidth) {
    EXPECT_EQ(0, calculateStride(0));
}

/* ====================================================================
 * sizeRatioPermille 测试
 * ==================================================================== */

TEST(OsdUtilsFontSizeLevelTest, SizeRatioPermille) {
    EXPECT_EQ(30, sizeRatioPermille("small"));
    EXPECT_EQ(44, sizeRatioPermille("medium"));
    EXPECT_EQ(60, sizeRatioPermille("large"));
    /* 未知档位回落 medium */
    EXPECT_EQ(44, sizeRatioPermille("huge"));
    EXPECT_EQ(44, sizeRatioPermille(""));
}

/* ====================================================================
 * snapFontTo16 测试
 * ==================================================================== */

TEST(OsdUtilsFontSnapTest, SnapFontTo16_BoundaryValues) {
    /* 刚好是 16 的倍数：不变 */
    EXPECT_EQ(16, snapFontTo16(16));
    EXPECT_EQ(32, snapFontTo16(32));
    EXPECT_EQ(48, snapFontTo16(48));
}

TEST(OsdUtilsFontSnapTest, SnapFontTo16_RoundUp) {
    /* 非 16 倍数：就近取整（四舍五入） */
    EXPECT_EQ(16, snapFontTo16(15));   /* 15+8=23, 23/16=1, 1*16=16 */
    EXPECT_EQ(16, snapFontTo16(17));   /* 17+8=25, 25/16=1, 1*16=16 */
    EXPECT_EQ(32, snapFontTo16(24));   /* 24+8=32, 32/16=2, 2*16=32 */
    EXPECT_EQ(32, snapFontTo16(25));   /* 25+8=33, 33/16=2, 2*16=32 */
}

TEST(OsdUtilsFontSnapTest, SnapFontTo16_MinimumIs16) {
    /* 小于 16 的值被拉到最小 16 */
    EXPECT_EQ(16, snapFontTo16(0));
    EXPECT_EQ(16, snapFontTo16(1));
    EXPECT_EQ(16, snapFontTo16(8));
    EXPECT_EQ(16, snapFontTo16(15));
    EXPECT_EQ(16, snapFontTo16(-10));  /* 负数也拉到 16 */
}

TEST(OsdUtilsFontSnapTest, SnapFontTo16_LargeValues) {
    EXPECT_EQ(64, snapFontTo16(64));
    EXPECT_EQ(64, snapFontTo16(60));
    EXPECT_EQ(80, snapFontTo16(75));
}

/* ====================================================================
 * fontSizeFromLevel 测试
 * ==================================================================== */

TEST(OsdUtilsFontSizeFromLevelTest, TypicalResolutions) {
    /* 1280x720：small=16px, medium=32px, large=48px */
    EXPECT_EQ(16, fontSizeFromLevel("small", 720));   /* 720*30/1000=21.6 -> snap to 16 */
    EXPECT_EQ(32, fontSizeFromLevel("medium", 720));  /* 720*44/1000=31.68 -> snap to 32 */
    EXPECT_EQ(48, fontSizeFromLevel("large", 720));   /* 720*60/1000=43.2 -> snap to 48 */
}

TEST(OsdUtilsFontSizeFromLevelTest, HighResolution) {
    /* 1920x1080 */
    EXPECT_EQ(32, fontSizeFromLevel("small", 1080));   /* 1080*30/1000=32.4 -> snap to 32 */
    EXPECT_EQ(48, fontSizeFromLevel("medium", 1080));  /* 1080*44/1000=47.52 -> snap to 48 */
    EXPECT_EQ(64, fontSizeFromLevel("large", 1080));   /* 1080*60/1000=64.8 -> snap to 64 */
}

TEST(OsdUtilsFontSizeFromLevelTest, LowResolution) {
    /* 640x480：最小分辨率 */
    int small = fontSizeFromLevel("small", 480);
    int medium = fontSizeFromLevel("medium", 480);
    int large = fontSizeFromLevel("large", 480);
    /* 480*30/1000=14.4 -> snap to 16 (最小值) */
    EXPECT_EQ(16, small);
    /* 480*44/1000=21.12 -> snap to 16 */
    EXPECT_EQ(16, medium);
    /* 480*60/1000=28.8 -> snap to 32 */
    EXPECT_EQ(32, large);
}

/* ====================================================================
 * parseColorName 测试
 * ==================================================================== */

/* 颜色比较辅助：Color 没有 operator== */
static bool colorEq(const Color& a, const Color& b) {
    return a.alpha == b.alpha && a.red == b.red &&
           a.green == b.green && a.blue == b.blue;
}

TEST(OsdUtilsColorParseTest, KnownColors) {
    EXPECT_TRUE(colorEq(Color::White(), parseColorName("white")));
    EXPECT_TRUE(colorEq(Color::Red(), parseColorName("red")));
    EXPECT_TRUE(colorEq(Color::Green(), parseColorName("green")));
    EXPECT_TRUE(colorEq(Color::Blue(), parseColorName("blue")));
    EXPECT_TRUE(colorEq(Color::Yellow(), parseColorName("yellow")));
    EXPECT_TRUE(colorEq(Color::Black(), parseColorName("black")));
}

TEST(OsdUtilsColorParseTest, UnknownColorFallsBackToBlack) {
    EXPECT_TRUE(colorEq(Color::Black(), parseColorName("purple")));
    EXPECT_TRUE(colorEq(Color::Black(), parseColorName("")));
    EXPECT_TRUE(colorEq(Color::Black(), parseColorName("WHITE")));  /* 大小写敏感 */
}
