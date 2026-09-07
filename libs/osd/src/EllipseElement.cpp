/**
 * EllipseElement.cpp - 椭圆元素实现 (归一化距离算法)
 * 
 * 功能说明：
 *   - 绘制椭圆边框（可配置边框宽度、颜色）
 *   - 支持可选的填充区域（可配置填充颜色）
 *   - 使用归一化距离算法判定椭圆边界
 *   - 使用像素级绘制（4bit色深，每像素4位）
 */

#include "OsdElement.h"
#include "OsdUtils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace osd {

/**
 * 默认构造函数
 * 
 * 功能说明：
 *   - 初始化椭圆为默认状态（中心(50,50)、rx=40、ry=30、白色边框、不填充）
 *   - 边框宽度默认为1像素
 */
EllipseElement::EllipseElement()
    : Element(ElementType::Ellipse)
    , center_x(50), center_y(50), rx(40), ry(30)
    , border_width(1), border_color(Color::White())
    , filled(false), fill_color(Color::Black())
    , alpha(0)
{
}

/**
 * 带配置的构造函数
 * 
 * 功能说明：
 *   - 根据配置参数初始化椭圆的所有属性
 *   - 支持中心坐标、X/Y半径、边框宽度、边框颜色、是否填充、填充颜色
 * 
 * @param config 椭圆配置（中心、半径、边框、填充等）
 */
EllipseElement::EllipseElement(const EllipseConfig& config)
    : Element(ElementType::Ellipse)
    , center_x(config.center_x), center_y(config.center_y)
    , rx(config.rx), ry(config.ry)
    , border_width(config.border_width)
    , border_color(config.border_color)
    , filled(config.filled)
    , fill_color(config.fill_color)
    , alpha(config.alpha)
{
}

/**
 * 创建OSD画布
 * 
 * 功能说明：
 *   - 根据中心坐标和半径计算画布包围盒（center±radius）
 *   - 调用clampToBounds()限制画布尺寸，防止越界（分辨率由引擎传入）
 *   - 组装pal::OsdCanvas经后端创建画布，创建成功后立即绘制初始椭圆
 *   - 如果元素启用，立即绘制初始椭圆
 * 
 * @param ch 视频通道号（0=主通道, 1=子通道）
 * @param rid OSD区域ID（0-3）
 * @param video_w 通道实际分辨率宽（引擎传入）
 * @param video_h 通道实际分辨率高
 * @return 0=创建成功, -1=创建失败
 */
int EllipseElement::create(int ch, int rid, int video_w, int video_h) {
    channel = ch;
    rect_id = rid;
    
    // 计算画布包围盒（以椭圆中心为中心，半径为边界）
    int w = rx * 2;  // 画布宽度=2*rx
    int h = ry * 2;  // 画布高度=2*ry
    int canvas_x = center_x - rx;  // 画布左上角X
    int canvas_y = center_y - ry;  // 画布左上角Y（video_w/video_h 由引擎传入）
    
    // 限制画布尺寸，防止超出视频帧范围
    CanvasBounds bounds = clampToBounds(canvas_x, canvas_y, w, h, video_w, video_h, video_w, video_h);
    
    // 配置OSD画布属性（平台无关描述，后端负责映射到硬件结构体）
    pal::OsdCanvas canvas;
    canvas.channel = ch;
    canvas.region_id = rid;
    canvas.x = bounds.x;
    canvas.y = bounds.y;
    canvas.width = bounds.width;
    canvas.height = bounds.height;
    canvas.bg_color_index = 0;  // 透明背景
    canvas.alpha = alpha < 0 ? 0 : (alpha > 100 ? 100 : alpha);   // 0~100，超范围钳住不拒绝
    
    // 经后端创建画布（硬件/软渲染由平台后端决定）
    if (!backend_ || !backend_->CreateCanvas(canvas)) {
        log(LogLevel::Error, "Failed to create ellipse canvas");
        return -1;
    }
    
    active = true;
    
    // 立即绘制初始椭圆（创建成功即首绘）
    return draw();
}

/**
 * 绘制椭圆
 * 
 * 功能说明：
 *   - 调用matchColorIndex()将颜色映射到硬件调色板索引（0-15）
 *   - 如果filled=true，先填充椭圆内部（使用归一化距离判定）
 *   - 使用归一化距离算法绘制椭圆边框（高效整数运算）
 *   - 经后端将像素缓冲区绘制到画布（平台差异归 pal 后端）
 * 
 * 算法说明：
 *   - 椭圆方程：(x²/rx²) + (y²/ry²) = 1
 *   - 归一化形式：x²*ry² + y²*rx² = rx²*ry²
 *   - 填充判定：x²*ry² + y²*rx² ≤ rx²*ry²（点在椭圆内）
 *   - 边框判定：|x²*ry² + y²*rx² - rx²*ry²| ≤ border_width*rx*ry
 *   - 使用long long避免溢出（rx²*ry²可能超出int范围）
 * 
 * @return 0=绘制成功, -1=绘制失败（元素未激活或内存分配失败）
 */
int EllipseElement::draw() {
    if (!active || !backend_) return -1;
    
    // 计算画布尺寸和缓冲区大小
    int w = rx * 2;  // 画布宽度=2*rx
    int h = ry * 2;  // 画布高度=2*ry
    
    int stride = calculateStride(w);  // 计算行步长（对齐）
    int pixels = stride * h;  // 总像素数
    int buf_len = (pixels + 1) / 2;  // 字节数（每字节2像素）
    
    // 分配像素缓冲区（4bit色深）
    unsigned char* dot_buf = (unsigned char*)calloc(1, buf_len);
    if (!dot_buf) return -1;
    
    // 将颜色映射到硬件调色板索引（0-15）
    int color_idx = matchColorIndex(border_color);
    int fill_idx = 0;
    bool should_fill = filled && fill_color.isOpaque();  // 检查是否需要填充
    if (should_fill) {
        fill_idx = matchColorIndex(fill_color);
    }
    
    // 预计算椭圆方程常量（使用long long避免溢出）
    long long rx2 = (long long)rx * rx;        // rx²
    long long ry2 = (long long)ry * ry;        // ry²
    long long rx2ry2 = rx2 * ry2;              // rx²*ry²
    long long rx_ry = (long long)rx * ry;      // rx*ry
    long long threshold = border_width * rx_ry;  // 边框宽度阈值
    
    int cx = rx, cy = ry;  // 椭圆中心在画布中的坐标
    
    // 步骤1：填充椭圆内部（如果启用填充）
    if (should_fill) {
        for (int py = 0; py < h; py++) {
            for (int px = 0; px < w; px++) {
                // 计算像素相对于椭圆中心的距离
                long long dx = px - cx;
                long long dy = py - cy;
                // 判定是否在椭圆内：x²*ry² + y²*rx² ≤ rx²*ry²
                if ((dx * dx * ry2 + dy * dy * rx2) <= rx2ry2) {
                    int pixel_idx = py * stride + px;
                    int byte_idx = pixel_idx / 2;
                    int bit_shift = (pixel_idx % 2 == 0) ? 4 : 0;
                    dot_buf[byte_idx] = (dot_buf[byte_idx] & ~(0x0F << bit_shift)) | (fill_idx << bit_shift);
                }
            }
        }
    }
    
    // 步骤2：绘制椭圆边框（使用归一化距离判定）
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            // 计算像素相对于椭圆中心的距离
            long long dx = px - cx;
            long long dy = py - cy;
            
            // 计算归一化距离值：x²*ry² + y²*rx²
            long long val = dx * dx * ry2 + dy * dy * rx2;
            long long diff = val - rx2ry2;  // 与椭圆方程的差值
            if (diff < 0) diff = -diff;     // 取绝对值
            
            // 判定是否在边框范围内：|差值| ≤ border_width*rx*ry
            if (diff <= threshold) {
                int pixel_idx = py * stride + px;
                int byte_idx = pixel_idx / 2;
                int bit_shift = (pixel_idx % 2 == 0) ? 4 : 0;
                // 设置像素颜色为边框颜色
                dot_buf[byte_idx] = (dot_buf[byte_idx] & ~(0x0F << bit_shift)) | (color_idx << bit_shift);
            }
        }
    }
    
    // 经后端将像素缓冲区绘制到画布（后端只读缓冲，绘制完即可释放）
    int ret = backend_->DrawBitmap(channel, rect_id, w, h, dot_buf, buf_len);
    free(dot_buf);  // 释放临时缓冲区
    
    return ret;
}

} // namespace osd
