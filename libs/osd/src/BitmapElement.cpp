/**
 * BitmapElement.cpp - 位图元素实现
 *
 * 把设备本地 BMP 图片贴上屏幕：
 *   读文件 → 解析 BMP 头（只认 24 位无压缩，不引任何图像库）
 *   → 最近邻采样缩到目标尺寸 → 每像素就近色匹配到硬件 16 色调色板
 *   → 填 4bpp 点阵走 DrawBitmap
 *
 * 为什么不用厂商示例里的亮度二值化：官方 sample 把 RGB 按亮度阈值
 * 拍成黑白两色，彩色 logo 会丢色；这里按欧氏距离就近匹配调色板，
 * 彩色能保住（代价是每像素多几次乘法，创建时一次性开销可忽略）。
 *
 * BMP 格式要点：像素从最后一行往上存（bottom-up），
 * 每行字节数补齐到 4 的倍数。
 */

#include "OsdElement.h"
#include "OsdUtils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace osd {

/**
 * 默认构造函数：空路径（仅为类型完整，实际用带配置的构造）
 */
BitmapElement::BitmapElement()
    : Element(ElementType::Bitmap)
    , x(0), y(0), width(0), height(0)
    , alpha(0)
{
}

/**
 * 从配置构造：只记路径和目标尺寸，不读文件
 *
 * 文件加载放在 draw：创建流程里建画布失败回滚时不浪费读文件开销。
 */
BitmapElement::BitmapElement(const BitmapConfig& config)
    : Element(ElementType::Bitmap)
    , image_path(config.image_path)
    , x(config.x), y(config.y)
    , width(config.width), height(config.height)
    , alpha(config.alpha)
{
}

/**
 * 读整个文件进内存（水印图片都很小，几十 KB 级别）
 *
 * @param elem 日志出口（文件读失败时打错误日志，为空则不打）
 * @return true=读成功（file 已填好），false=文件不存在/读失败/小于最小 BMP 长度
 */
static bool loadFile(const std::string& path, std::vector<unsigned char>& file,
                     Element* elem) {
    auto err = [elem](const std::string& m) {
        if (elem) elem->log(LogLevel::Error, m);
    };
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        err("Bitmap file not found: " + path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    const long file_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    /* 14(文件头) + 40(信息头) 是最小合法长度 */
    if (file_len < 54) {
        err("File too small to be a BMP");
        fclose(fp);
        return false;
    }
    file.resize((size_t)file_len);
    if (fread(file.data(), 1, file.size(), fp) != file.size()) {
        err("Bitmap file read failed");
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

/**
 * 建画布：按目标显示尺寸建，背景透明、不透明叠加（logo 要全显）
 *
 * 顺序敏感：文件读取与头部校验必须在建画布之前——画布建好后失败，
 * 引擎回滚只删元素不销画布，区域会泄漏（同槽位再也建不出画布）。
 *
 * @param ch 视频通道号（0=主 1=子）
 * @param rid 区域ID（按配置顺序分配）
 * @param video_w/video_h 通道实际分辨率（画布钳位基准）
 * @return 0=成功, -1=失败（尺寸非法/文件缺失/格式不支持/建画布失败）
 */
int BitmapElement::create(int ch, int rid, int video_w, int video_h) {
    channel = ch;
    rect_id = rid;

    if (width <= 0 || height <= 0) {
        log(LogLevel::Error, "Bitmap target size must be positive");
        return -1;
    }
    if (image_path.empty()) {
        log(LogLevel::Error, "Bitmap image path is empty");
        return -1;
    }
    /* 先验证文件可用再碰画布：失败时什么都没建，回滚零负担 */
    std::vector<unsigned char> file;
    if (!loadFile(image_path, file, this)) {
        return -1;
    }

    /* 钳位兜底：上层（OsdService）已按 max_rect 钳过，这里防漏 */
    CanvasBounds bounds = clampToBounds(x, y, width, height,
                                        video_w, video_h, video_w, video_h);

    pal::OsdCanvas canvas;
    canvas.channel = ch;
    canvas.region_id = rid;
    canvas.x = bounds.x;
    canvas.y = bounds.y;
    canvas.width = bounds.width;
    canvas.height = bounds.height;
    canvas.bg_color_index = 0;  /* 背景透明（图片没盖到的地方露出画面） */
    /* 透明度 0~100（0=不透明）：默认 0 全显，配置可调半透 */
    canvas.alpha = alpha < 0 ? 0 : (alpha > 100 ? 100 : alpha);

    if (!backend_ || !backend_->CreateCanvas(canvas)) {
        log(LogLevel::Error, "Failed to create bitmap canvas");
        return -1;
    }

    active = true;
    /* 画布被钳小时同步目标尺寸，采样按钳位后的尺寸来 */
    x = bounds.x;
    y = bounds.y;
    width = bounds.width;
    height = bounds.height;

    return draw();
}

/**
 * 读 BMP + 量化 + 贴画布（静态元素只在创建时画这一次；
 * 经引擎重绘再进来会重读文件重画一遍，幂等）
 *
 * @return 0=成功, -1=失败（文件打不开/格式不支持/采样失败）
 */
int BitmapElement::draw() {
    if (!active || !backend_) return -1;

    /* ---- 读整个文件进内存 ---- */
    std::vector<unsigned char> file;
    if (!loadFile(image_path, file, this)) {
        return -1;
    }
    const long file_len = (long)file.size();

    /* ---- 解析 BMP 头（小端格式，逐字节拼避免结构体对齐问题） ---- */
    const unsigned char* p = file.data();
    if (p[0] != 'B' || p[1] != 'M') {
        log(LogLevel::Error, "Not a BMP file (bad magic)");
        return -1;
    }
    auto rd32 = [&p](int off) {
        return (int)((unsigned)p[off] | ((unsigned)p[off + 1] << 8) |
                     ((unsigned)p[off + 2] << 16) | ((unsigned)p[off + 3] << 24));
    };
    const int pixel_offset = rd32(10);       /* 像素数据起始偏移 */
    const int img_w = rd32(18);              /* 图片宽 */
    const int img_h = rd32(22);              /* 图片高（>0 表示从下往上存） */
    const int bit_count = (int)((unsigned)p[28] | ((unsigned)p[29] << 8));
    const int compression = rd32(30);

    if (bit_count != 24 || compression != 0 || img_w <= 0 || img_h <= 0) {
        /* 只支持 24 位无压缩：压缩位图和索引色位图解码复杂度不值得 */
        log(LogLevel::Error, "Unsupported BMP format (need 24-bit uncompressed)");
        return -1;
    }
    if (pixel_offset <= 0 || pixel_offset >= file_len) {
        log(LogLevel::Error, "Bad BMP pixel offset");
        return -1;
    }
    /* 防恶意文件：按头部尺寸算出来的数据量不能超文件实际大小 */
    const int line_bytes = (img_w * 3 + 3) & ~3;   /* 每行补齐 4 字节 */
    if ((long)line_bytes * img_h > file_len - pixel_offset) {
        log(LogLevel::Error, "BMP pixel data truncated");
        return -1;
    }

    /* ---- 最近邻采样缩到目标尺寸，每像素就近色匹配，填 4bpp 点阵 ---- */
    const int stride = calculateStride(width);
    const int buf_len = (stride * height + 1) / 2;
    unsigned char* dot_buf = (unsigned char*)calloc(1, buf_len);
    if (!dot_buf) return -1;

    const unsigned char* pix = file.data() + pixel_offset;
    for (int dy = 0; dy < height; ++dy) {
        /* BMP 从最后一行存起：目标第 0 行对应源图最后一行 */
        const int sy = img_h - 1 - dy * img_h / height;
        const unsigned char* row = pix + (long)sy * line_bytes;
        for (int dx = 0; dx < width; ++dx) {
            const int sx = dx * img_w / width;
            const unsigned char* bgr = row + sx * 3;   /* BMP 像素序是 B,G,R */
            /* matchColorIndex 内部做调色板欧氏距离就近匹配（含透明返回 0） */
            const int idx = matchColorIndex(Color(255, bgr[2], bgr[1], bgr[0]));

            const int pixel_idx = dy * stride + dx;
            const int byte_idx = pixel_idx / 2;
            const int bit_shift = (pixel_idx % 2 == 0) ? 4 : 0;
            dot_buf[byte_idx] = (dot_buf[byte_idx] & ~(0x0F << bit_shift)) |
                                (idx << bit_shift);
        }
    }

    int ret = backend_->DrawBitmap(channel, rect_id, width, height,
                                   dot_buf, buf_len);
    free(dot_buf);
    if (ret != 0) {
        log(LogLevel::Error, "Bitmap DrawBitmap failed");
    }
    return ret;
}

} // namespace osd
