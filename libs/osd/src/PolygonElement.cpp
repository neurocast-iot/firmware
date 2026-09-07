/**
 * PolygonElement.cpp - 多边形元素实现
 *
 * 硬件叠加器只认索引色位图，没有画多边形的接口，
 * 这里用扫描线填充把任意多边形栅格化成 4bpp 点阵，
 * 再走和矩形/椭圆一样的 DrawBitmap 管线上屏。
 *
 * 填充算法：逐行扫描线 + 奇偶规则——
 * 每条扫描线和所有边求交点，交点排序后两两配对填区间；
 * 奇偶规则天然支持凹多边形，不需要拆三角形。
 */

#include "OsdElement.h"
#include "OsdUtils.h"

#include <algorithm>
#include <stdlib.h>
#include <vector>

namespace osd {

/**
 * 默认构造函数：空顶点列表（不画任何东西，仅为类型完整）
 */
PolygonElement::PolygonElement()
    : Element(ElementType::Polygon)
    , fill_color(Color::Black())
    , alpha(20)
{
}

/**
 * 从配置构造：拷贝顶点列表和填充色
 *
 * 顶点是像素绝对坐标（屏幕坐标系），包围盒在 create 时才算，
 * 因为钳位需要知道通道分辨率，构造时还拿不到。
 */
PolygonElement::PolygonElement(const PolygonConfig& config)
    : Element(ElementType::Polygon)
    , points(config.points)
    , fill_color(config.fill_color)
    , alpha(config.alpha)
{
}

/**
 * 建画布：算顶点包围盒当画布位置和尺寸
 *
 * 画布只盖住多边形外接矩形，不浪费叠加区域面积
 * （区域尺寸受限，能省则省）。顶点少于 3 个直接失败。
 *
 * @param ch 视频通道号（0=主 1=子）
 * @param rid 区域ID（按配置顺序分配，平台容量见后端）
 * @param video_w/video_h 通道实际分辨率（画布钳位基准）
 * @return 0=成功, -1=失败（顶点不足或建画布失败）
 */
int PolygonElement::create(int ch, int rid, int video_w, int video_h) {
    channel = ch;
    rect_id = rid;

    if (points.size() < 3) {
        log(LogLevel::Error, "Polygon needs at least 3 points");
        return -1;
    }

    /* 算顶点包围盒：画布位置和尺寸都从这来 */
    int min_x = points[0].x, max_x = points[0].x;
    int min_y = points[0].y, max_y = points[0].y;
    for (const auto& p : points) {
        if (p.x < min_x) min_x = p.x;
        if (p.x > max_x) max_x = p.x;
        if (p.y < min_y) min_y = p.y;
        if (p.y > max_y) max_y = p.y;
    }
    const int bw = max_x - min_x + 1;
    const int bh = max_y - min_y + 1;

    /* 防越界：位置/尺寸钳进通道分辨率（上层已钳过一道，这里是兜底） */
    CanvasBounds bounds = clampToBounds(min_x, min_y, bw, bh,
                                        video_w, video_h, video_w, video_h);
    if (bounds.width <= 0 || bounds.height <= 0) {
        log(LogLevel::Error, "Polygon bounding box degenerate after clamp");
        return -1;
    }

    pal::OsdCanvas canvas;
    canvas.channel = ch;
    canvas.region_id = rid;
    canvas.x = bounds.x;
    canvas.y = bounds.y;
    canvas.width = bounds.width;
    canvas.height = bounds.height;
    canvas.bg_color_index = 0;  /* 多边形外区域透明，只露出填充部分 */
    /* 透明度 0~100（0=不透明）：和矩形遮盖同语义默认 20，可由配置调；
     * 旧代码写 51 是按 0~255 直觉写的，硬件实际按百分比算，已修正 */
    canvas.alpha = alpha < 0 ? 0 : (alpha > 100 ? 100 : alpha);

    if (!backend_ || !backend_->CreateCanvas(canvas)) {
        log(LogLevel::Error, "Failed to create polygon canvas");
        return -1;
    }

    active = true;

    /* 记下钳位后的画布（draw 按它做坐标平移和裁剪） */
    x_ = bounds.x;
    y_ = bounds.y;
    canvas_w_ = bounds.width;
    canvas_h_ = bounds.height;

    return draw();
}

/**
 * 扫描线填充画多边形
 *
 * 逐行算交点 → 排序 → 两两配对填区间，4bpp 点阵交给后端贴画布。
 * 静态元素只在创建时画这一次，不进刷新线程。
 *
 * @return 0=成功, -1=失败
 */
int PolygonElement::draw() {
    if (!active || !backend_) return -1;

    const int fill_idx = matchColorIndex(fill_color);
    const int stride = calculateStride(canvas_w_);
    const int buf_len = (stride * canvas_h_ + 1) / 2;  /* 每字节 2 像素 */

    unsigned char* dot_buf = (unsigned char*)calloc(1, buf_len);
    if (!dot_buf) return -1;

    /* 写一个像素的颜色索引（偶数像素在高 4 位，奇数在低 4 位） */
    auto putPixel = [&](int px, int py, int idx) {
        const int pixel_idx = py * stride + px;
        const int byte_idx = pixel_idx / 2;
        const int bit_shift = (pixel_idx % 2 == 0) ? 4 : 0;
        dot_buf[byte_idx] = (dot_buf[byte_idx] & ~(0x0F << bit_shift)) |
                            (idx << bit_shift);
    };

    /* 逐行扫描：行号是画布局部坐标，换回屏幕坐标再和顶点比 */
    std::vector<int> xs;
    const size_t n = points.size();
    for (int row = 0; row < canvas_h_; ++row) {
        const int scan_y = y_ + row;
        xs.clear();

        /* 和每条边求交：边跨扫描线才算（半开区间，避免顶点处重复计数） */
        for (size_t i = 0; i < n; ++i) {
            const Point& p1 = points[i];
            const Point& p2 = points[(i + 1) % n];
            const bool crosses = (p1.y <= scan_y && p2.y > scan_y) ||
                                 (p2.y <= scan_y && p1.y > scan_y);
            if (!crosses) continue;
            /* 线性插值求交点 x（整数运算，误差在半像素内可接受） */
            const int ix = p1.x + (scan_y - p1.y) * (p2.x - p1.x) / (p2.y - p1.y);
            xs.push_back(ix);
        }
        if (xs.size() < 2) continue;   /* 本行没穿过图形 */

        std::sort(xs.begin(), xs.end());

        /* 奇偶规则：交点两两配对，每对之间是图形内部 */
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            /* 交点钳进画布范围（画布被钳位过时顶点可能落在外面） */
            int x0 = xs[i] - x_;
            int x1 = xs[i + 1] - x_;
            if (x0 < 0) x0 = 0;
            if (x1 >= canvas_w_) x1 = canvas_w_ - 1;
            if (x0 > x1) continue;
            for (int px = x0; px <= x1; ++px) {
                putPixel(px, row, fill_idx);
            }
        }
    }

    int ret = backend_->DrawBitmap(channel, rect_id, canvas_w_, canvas_h_,
                                   dot_buf, buf_len);
    free(dot_buf);
    return ret;
}

} // namespace osd
