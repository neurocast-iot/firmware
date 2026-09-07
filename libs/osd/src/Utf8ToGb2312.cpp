// UTF-8 → GB2312 宽字符转换（OSD 文本渲染专用）
//
// 背景：云端下发的文本是 UTF-8，平台画字接口与字库
// 按 GB2312 码位索引，设备上无 iconv（uClibc 不带），只能内嵌查找表。
// 表由 scripts/gen_gb2312_table.py 生成（约 7400 条，27KB）。
//
// 本文件是 OsdUtils.h 里原 convertToWideChar 的替代：原实现按 GBK
// 两字节盲配对，解析 UTF-8 中文会把相邻字符的字节拼成乱码字
// （实测 "1号机" 画成 "1锋"）。
#include "OsdUtils.h"

#include <cstdint>

namespace osd {

/* 生成表：每条高16位=Unicode 码点，低16位=GB2312 码，按 Unicode 升序 */
#include "gb2312_unicode_table.inc"

/**
 * Unicode 码点查 GB2312 码（二分）
 *
 * @return 查到返回 GB2312 两字节码，查不到（字库外的字）返回 0
 */
static uint16_t lookupGb2312(uint32_t codepoint) {
    int lo = 0;
    int hi = (int)(sizeof(kUnicodeToGb2312) / sizeof(kUnicodeToGb2312[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t cp = kUnicodeToGb2312[mid] >> 16;
        if (cp == codepoint) {
            return (uint16_t)(kUnicodeToGb2312[mid] & 0xFFFF);
        }
        if (cp < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
}

/**
 * 解一个 UTF-8 字符
 *
 * @param[out] codepoint 解出的 Unicode 码点
 * @return 消耗的字节数；0 表示非法字节（调用方跳过 1 字节）
 *
 * 注意：多字节分支会看后续字节，调用方保证缓冲比字符串长出
 * 至少 3 字节（实际调用都是定长数组+memset，满足）
 */
static int decodeUtf8(const unsigned char* s, uint32_t* codepoint) {
    if (s[0] < 0x80) {
        *codepoint = s[0];
        return 1;
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(s[0] & 0x0F) << 12) |
                     ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(s[0] & 0x07) << 18) |
                     ((uint32_t)(s[1] & 0x3F) << 12) |
                     ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return 0;
}

/**
 * 字符串转换为宽字符（输入按 UTF-8 解析，输出 GB2312 码）
 *
 * 为什么输出是 GB2312 码而不是 Unicode：画字接口和字库都按
 * GB2312 码位找字形，Unicode 码点传进去找不到字。
 *
 * 宽字符的拼法有讲究：SDK 内部按内存字节序读这两个字节，
 * ARM 小端下要让区字节（高字节）落在低地址，所以宽字符值
 * 得拼成 (位字节<<8)|区字节——与旧实现 GBK 配对的拼法一致。
 *
 * 异常处理：非法字节跳过；字库外的字（emoji/生僻字）替换成 '?'，
 * 避免画出乱码块。
 */
int convertToWideChar(uint16_t* dest, const char* src, int max_len) {
    int count = 0;
    const unsigned char* s = (const unsigned char*)src;

    while (*s && count < max_len) {
        uint32_t cp = 0;
        int n = decodeUtf8(s, &cp);
        if (n == 0) {
            /* 非法字节：跳过 1 字节继续，不中断整串转换 */
            s += 1;
            continue;
        }
        s += n;

        if (cp < 0x80) {
            /* ASCII 直通 */
            *dest++ = (uint16_t)cp;
            count++;
            continue;
        }

        uint16_t gb = lookupGb2312(cp);
        if (gb == 0) {
            /* 字库外字符替换 '?'，保证输出可读不画乱码 */
            *dest++ = (uint16_t)'?';
            count++;
            continue;
        }
        /* GB2312 两字节码：区=高字节、位=低字节；
         * 小端内存下低地址放区字节，宽字符值 = (位<<8)|区 */
        *dest++ = (uint16_t)(((gb & 0xFF) << 8) | ((gb >> 8) & 0xFF));
        count++;
    }

    return count;
}

} // namespace osd
