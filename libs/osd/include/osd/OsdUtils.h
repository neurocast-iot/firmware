/**
 * OsdUtils.h - OSD工具函数（全内联，嵌入式优化）
 * 
 * 功能：
 *   - 颜色匹配算法
 *   - 字符编码转换
 *   - 字体大小计算
 *   - 画布边界计算
 * 
 * 设计原则：
 *   - 颜色/字体类小函数标记 inline，编译器直接展开，零调用开销
 *   - constexpr常量，编译期求值
 *   - 无状态纯函数，不修改外部状态
 *   - 零平台依赖：不引任何厂商 SDK 头（字体刷新已下沉 pal 后端）
 */

#ifndef OSD_UTILS_H
#define OSD_UTILS_H

#include "OsdTypes.h"

namespace osd {

/**
 * 匹配颜色到硬件颜色表索引
 * 
 * @param color 输入颜色
 * @return 颜色表索引 (0-15)
 * 
 * 算法：
 *   1. 透明色直接返回0
 *   2. 精确匹配常用色（白、黑、红、绿、蓝等）
 *   3. 最邻近匹配（欧氏距离最小）
 */
inline int matchColorIndex(const Color& color) {
    // 透明色
    if (color.alpha == 0) {
        return 0;
    }
    
    // 精确匹配常用色（8种）- 必须RGB值完全相等
    if (color.red == 255 && color.green == 255 && color.blue == 255) return 1;  // 白色
    if (color.red == 0 && color.green == 0 && color.blue == 0) return 2;        // 黑色
    if (color.red == 255 && color.green == 0 && color.blue == 0) return 3;      // 红色
    if (color.red == 0 && color.green == 255 && color.blue == 0) return 4;      // 绿色
    if (color.red == 0 && color.green == 0 && color.blue == 255) return 5;      // 蓝色
    if (color.red == 255 && color.green == 255 && color.blue == 0) return 6;    // 黄色
    if (color.red == 255 && color.green == 0 && color.blue == 255) return 7;    // 青色
    if (color.red == 0 && color.green == 255 && color.blue == 255) return 8;    // 洋红
    
    // 最邻近匹配（欧氏距离）
    int best_idx = 1;
    unsigned int best_dist = 0xFFFFFFFF;
    
    for (int i = 0; i < 16; i++) {
        uint32_t tbl = HARDWARE_PALETTE[i];
        // ABGR8888格式：R在最低字节，B在最高有效RGB字节
        int r = tbl & 0xFF;           // ABGR: R在最低字节
        int g = (tbl >> 8) & 0xFF;    // ABGR: G在第二字节
        int b = (tbl >> 16) & 0xFF;   // ABGR: B在第三字节
        
        int dr = color.red - r;
        int dg = color.green - g;
        int db = color.blue - b;
        
        unsigned int dist = dr * dr + dg * dg + db * db;
        
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    
    return best_idx;
}

/**
 * 字符串转换为宽字符（输入按 UTF-8 解析，输出 GB2312 码）
 *
 * @param dest 目标宽字符缓冲（每个宽字符存一个 GB2312 两字节码）
 * @param src 源字符串（UTF-8，云端/JSON 下发的文本都是这个编码）
 * @param max_len 最大字符数
 * @return 转换的字符数
 *
 * 实现在 src/Utf8ToGb2312.cpp：内嵌 Unicode→GB2312 查找表（设备无 iconv）。
 * 历史教训：旧实现按 GBK 两字节盲配对，UTF-8 中文会被拼成乱码字。
 */
int convertToWideChar(uint16_t* dest, const char* src, int max_len);

/**
 * 根据视频宽度计算字体大小
 * 
 * @param video_width 视频宽度
 * @return 推荐的字体大小
 * 
 * 规则：
 *   - 2560+ -> 64px
 *   - 1920  -> 48px
 *   - 1024  -> 32px
 *   - 960   -> 24px
 *   - 其他  -> 16px
 */
inline int calculateFontSize(int video_width) {
    if (video_width >= 2560) return 64;
    if (video_width >= 1920) return 48;
    if (video_width >= 1024) return 32;
    if (video_width >= 960)  return 24;
    return 16;
}

/* 文本画布末尾的边缘保护（像素）：防最后一个字的边缘像素被裁掉。
 * SDK 头文件要求每字额外留 2 像素，4 像素已留富余且实测不裁边。
 * 注意这是前后端共同约定的数：前端贴右公式里减的也是它，
 * 改动必须前后端同步，否则贴右摆放会离边过远或末字被裁 */
constexpr int kTextEdgePadPx = 4;

/**
 * 统计字符串的"字宽单位数"（半角位个数）
 *
 * @param text 文本内容（UTF-8）
 * @return ASCII 字符算 1 个单位，多字节字符（中文等）算 2 个单位；
 *         画布宽度 = 单位数 × 字号/2（点阵字是等宽的：
 *         ASCII 宽 = 字号/2，中文宽 = 字号）
 *
 * 按 UTF-8 数"字"而不是数字节：多字节字符整体算 2 个单位，
 * 续字节（0x80-0xBF）不重复计数——按字节算的话，一个中文 3 字节
 * 会被算成 6 个单位，画布宽出去两倍多。
 * 所有输入（含非法字节）的估值都 ≥ 实际渲染宽度，画布只会偏宽不会裁边。
 * OsdService 的画布预检也调这个函数，宽度口径只此一份。
 */
inline int utf8WidthUnits(const std::string& text) {
    int units = 0;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(text.c_str());
    size_t i = 0;
    while (i < text.size()) {
        if (s[i] < 0x80) {
            units += 1;                     /* ASCII：半个字宽 */
            ++i;
        } else if (s[i] >= 0xC0) {
            units += 2;                     /* 多字节字符（中文等）：整个字算两个半角位 */
            ++i;
            while (i < text.size() && (s[i] & 0xC0) == 0x80) ++i;  /* 跳过后续续字节 */
        } else {
            ++i;                            /* 孤立的续字节：不计宽，只跳过 */
        }
    }
    return units;
}

/**
 * 计算文本画布宽度（像素）
 *
 * @param text 文本内容（UTF-8）
 * @param font_size 字体大小（像素）
 * @return 画布宽度（像素）= 字宽 + 末尾边缘保护（kTextEdgePadPx）
 */
inline int calculateTextWidth(const std::string& text, int font_size) {
    return font_size / 2 * utf8WidthUnits(text) + kTextEdgePadPx;
}

/**
 * 画布边界结构
 */
struct CanvasBounds {
    int x;
    int y;
    int width;
    int height;
};

/**
 * 限制画布边界（防止越界）
 * 
 * @param x 原始X坐标
 * @param y 原始Y坐标
 * @param w 原始宽度
 * @param h 原始高度
 * @param max_w 最大宽度（SDK限制）
 * @param max_h 最大高度（SDK限制）
 * @param video_w 视频宽度
 * @param video_h 视频高度
 * @return 修正后的边界
 */
inline CanvasBounds clampToBounds(
    int x, int y, int w, int h,
    int max_w, int max_h,
    int video_w, int video_h)
{
    CanvasBounds bounds;
    
    // 限制尺寸不超过SDK最大值
    if (max_w > 0 && w > max_w) w = max_w;
    if (max_h > 0 && h > max_h) h = max_h;
    
    // 确保坐标不为负
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    
    // 确保不超出视频边界
    if (x + w > video_w) x = video_w - w;
    if (y + h > video_h) y = video_h - h;
    
    bounds.x = x;
    bounds.y = y;
    bounds.width = w;
    bounds.height = h;
    
    return bounds;
}

/**
 * 计算行对齐步长（避免奇数宽度导致的对角线）
 * 
 * @param width 原始宽度
 * @return 对齐后的步长（向上取整到偶数）
 */
inline int calculateStride(int width) {
    return (width + 1) & ~1;  // 向上取整到偶数
}

/* ---- 字号档位换算（点阵字库约束） ---- */

/**
 * 字号档位 → 字高占画面高度的千分比
 *
 * @param size 字号档位（"small" / "medium" / "large"，配置层已保证只有这三档）
 * @return 千分比值（small=30, medium=44, large=60）
 *
 * 1280x720 上：small=16px、medium=32px、large=48px，和旧配置观感对齐
 */
inline int sizeRatioPermille(const std::string& size) {
    if (size == "small") return 30;
    if (size == "large") return 60;
    return 44;   /* medium */
}

/**
 * 字号就近取 16 的整倍数
 *
 * @param rawPx 原始像素值
 * @return 对齐后的像素值（最小 16）
 *
 * 字库是 16px 点阵，整倍放大才清晰，非整倍数（如 20/28）会发虚破边
 */
inline int snapFontTo16(int rawPx) {
    int snapped = ((rawPx + 8) / 16) * 16;
    return snapped < 16 ? 16 : snapped;
}

/**
 * 档位字号 → 像素：画面高 × 千分比 → 取档
 *
 * @param size 字号档位（"small" / "medium" / "large"）
 * @param channelHeight 通道实际高度（像素）
 * @return 对齐后的字体像素值
 */
inline int fontSizeFromLevel(const std::string& size, int channelHeight) {
    return snapFontTo16(channelHeight * sizeRatioPermille(size) / 1000);
}

/**
 * 配置颜色名 → 库颜色
 *
 * @param name 颜色名（"black"/"white"/"red"/"green"/"blue"/"yellow"）
 * @return 对应的 Color 对象，非法名回落 black
 *
 * 限硬件调色板内的 6 色，非法名配置层已回落 black
 */
inline Color parseColorName(const std::string& name) {
    if (name == "white")  return Color::White();
    if (name == "red")    return Color::Red();
    if (name == "green")  return Color::Green();
    if (name == "blue")   return Color::Blue();
    if (name == "yellow") return Color::Yellow();
    return Color::Black();
}

} // namespace osd

#endif // OSD_UTILS_H
