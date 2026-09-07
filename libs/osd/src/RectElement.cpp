/**
 * RectElement.cpp - 矩形元素实现
 * 
 * 功能说明：
 *   - 绘制矩形边框（可配置边框宽度、颜色）
 *   - 支持可选的填充区域（可配置填充颜色）
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
 *   - 初始化矩形为默认状态（100x100、白色边框、不填充）
 *   - 边框宽度默认为1像素
 */
RectElement::RectElement()
    : Element(ElementType::Rect)
    , x(0), y(0), width(100), height(100)
    , border_width(1), border_color(Color::White())
    , filled(false), fill_color(Color::Black())
    , alpha(20)
{
}

/**
 * 带配置的构造函数
 * 
 * 功能说明：
 *   - 根据配置参数初始化矩形的所有属性
 *   - 支持边框宽度、边框颜色、是否填充、填充颜色
 * 
 * @param config 矩形配置（位置、尺寸、边框、填充等）
 */
RectElement::RectElement(const RectConfig& config)
    : Element(ElementType::Rect)
    , x(config.x), y(config.y)
    , width(config.width), height(config.height)
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
 *   - 调用clampToBounds()限制画布尺寸，防止越界（分辨率由引擎传入）
 *   - 组装pal::OsdCanvas经后端创建画布，创建成功后立即绘制初始矩形
 * 
 * @param ch 视频通道号（0=主通道, 1=子通道）
 * @param rid OSD区域ID（0-3）
 * @param video_w 通道实际分辨率宽（引擎传入）
 * @param video_h 通道实际分辨率高
 * @return 0=创建成功, -1=创建失败
 */
int RectElement::create(int ch, int rid, int video_w, int video_h) {
    channel = ch;
    rect_id = rid;
    
    // 限制画布尺寸，防止超出视频帧范围
    CanvasBounds bounds = clampToBounds(x, y, width, height, video_w, video_h, video_w, video_h);
    
    // 配置OSD画布属性（平台无关描述，后端负责映射到硬件结构体）
    pal::OsdCanvas canvas;
    canvas.channel = ch;
    canvas.region_id = rid;
    canvas.x = bounds.x;
    canvas.y = bounds.y;
    canvas.width = bounds.width;
    canvas.height = bounds.height;
    canvas.bg_color_index = 0;  // 透明背景
    /* 透明度 0~100（SDK ak_osd_set_alpha 注释的量纲，0=不透明）：
     * 旧代码写 51 是按 0~255 直觉写的，实际硬件按百分比算偏透，已修正 */
    canvas.alpha = alpha < 0 ? 0 : (alpha > 100 ? 100 : alpha);
    
    // 经后端创建画布（硬件/软渲染由平台后端决定）
    if (!backend_ || !backend_->CreateCanvas(canvas)) {
        log(LogLevel::Error, "Failed to create rect canvas");
        return -1;
    }
    
    active = true;
    
    // 立即绘制初始矩形
    return draw();
}

/**
 * 绘制矩形
 * 
 * 功能说明：
 *   - 调用matchColorIndex()将颜色映射到硬件调色板索引（0-15）
 *   - 计算像素缓冲区大小（4bit色深，每字节存储2个像素）
 *   - 遍历所有像素，根据位置判断是边框还是填充区域
 *   - 经后端将像素缓冲区绘制到画布（平台差异归 pal 后端）
 * 
 * 算法说明：
 *   - 边框判定：像素距离矩形边缘 < border_width
 *   - 填充判定：filled=true 且 fill_color不透明
 *   - 像素存储：每字节2个像素，高4位=偶数像素，低4位=奇数像素
 * 
 * @return 0=绘制成功, -1=绘制失败（元素未激活或内存分配失败）
 */
int RectElement::draw() {
    if (!active || !backend_) return -1;
    
    // 将颜色映射到硬件调色板索引（0-15）
    int color_idx = matchColorIndex(border_color);
    int fill_idx = 0;
    bool should_fill = filled && fill_color.isOpaque();  // 检查是否需要填充
    if (should_fill) {
        fill_idx = matchColorIndex(fill_color);
    }
    
    // 计算stride对齐（确保每行字节数对齐）
    int stride = calculateStride(width);
    int pixels = stride * height;  // 总像素数
    int buf_len = (pixels + 1) / 2;  // 字节数（每字节2像素）
    
    // 分配像素缓冲区（4bit色深）
    unsigned char* dot_buf = (unsigned char*)calloc(1, buf_len);
    if (!dot_buf) return -1;
    
    // 遍历所有像素，填充像素缓冲区
    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            // 判定是否为边框区域（距离边缘 < border_width）
            bool is_border = (py < border_width || py >= height - border_width ||
                             px < border_width || px >= width - border_width);
            
            // 计算像素在缓冲区中的位置
            int pixel_idx = py * stride + px;  // 像素索引
            int byte_idx = pixel_idx / 2;      // 字节索引
            int bit_shift = (pixel_idx % 2 == 0) ? 4 : 0;  // 位偏移（偶数=高4位，奇数=低4位）
            
            // 根据位置填充颜色索引
            if (is_border) {
                // 边框区域：使用边框颜色
                dot_buf[byte_idx] = (dot_buf[byte_idx] & ~(0x0F << bit_shift)) | (color_idx << bit_shift);
            } else if (should_fill) {
                // 内部区域：使用填充颜色
                dot_buf[byte_idx] = (dot_buf[byte_idx] & ~(0x0F << bit_shift)) | (fill_idx << bit_shift);
            }
        }
    }
    
    // 经后端将像素缓冲区绘制到画布（后端只读缓冲，绘制完即可释放）
    int ret = backend_->DrawBitmap(channel, rect_id, width, height, dot_buf, buf_len);
    free(dot_buf);  // 释放临时缓冲区
    
    return ret;
}

} // namespace osd
