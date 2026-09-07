/**
 * CircleElement.cpp - 圆形元素实现 (Bresenham算法)
 * 
 * 功能说明：
 *   - 绘制圆形边框（可配置边框宽度、颜色）
 *   - 支持可选的填充区域（可配置填充颜色）
 *   - 使用Bresenham算法高效绘制圆形边框
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
 *   - 初始化圆形为默认状态（圆心(50,50)、半径40、白色边框、不填充）
 *   - 边框宽度默认为1像素
 */
CircleElement::CircleElement()
    : Element(ElementType::Circle)
    , center_x(50), center_y(50), radius(40)
    , border_width(1), border_color(Color::White())
    , filled(false), fill_color(Color::Black())
    , alpha(0)
{
}

/**
 * 带配置的构造函数
 * 
 * 功能说明：
 *   - 根据配置参数初始化圆形的所有属性
 *   - 支持圆心坐标、半径、边框宽度、边框颜色、是否填充、填充颜色
 * 
 * @param config 圆形配置（圆心、半径、边框、填充等）
 */
CircleElement::CircleElement(const CircleConfig& config)
    : Element(ElementType::Circle)
    , center_x(config.center_x), center_y(config.center_y)
    , radius(config.radius)
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
 *   - 根据圆心和半径计算画布包围盒（center±radius）
 *   - 调用clampToBounds()限制画布尺寸，防止越界（分辨率由引擎传入）
 *   - 组装pal::OsdCanvas经后端创建画布，创建成功后立即绘制初始圆形
 * 
 * @param ch 视频通道号（0=主通道, 1=子通道）
 * @param rid OSD区域ID（0-3）
 * @param video_w 通道实际分辨率宽（引擎传入）
 * @param video_h 通道实际分辨率高
 * @return 0=创建成功, -1=创建失败
 */
int CircleElement::create(int ch, int rid, int video_w, int video_h) {
    channel = ch;
    rect_id = rid;
    
    // 计算画布包围盒（以圆心为中心，半径为边界）
    int size = radius * 2;  // 画布尺寸=直径
    int canvas_x = center_x - radius;  // 画布左上角X
    int canvas_y = center_y - radius;  // 画布左上角Y（video_w/video_h 由引擎传入）
    
    // 限制画布尺寸，防止超出视频帧范围
    CanvasBounds bounds = clampToBounds(canvas_x, canvas_y, size, size, video_w, video_h, video_w, video_h);
    
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
        log(LogLevel::Error, "Failed to create circle canvas");
        return -1;
    }
    
    active = true;
    
    // 立即绘制初始圆形
    return draw();
}

/**
 * 绘制圆形
 * 
 * 功能说明：
 *   - 调用matchColorIndex()将颜色映射到硬件调色板索引（0-15）
 *   - 如果filled=true，先填充圆形内部（使用距离判定法）
 *   - 使用Bresenham算法绘制圆形边框（高效整数运算）
 *   - 经后端将像素缓冲区绘制到画布（平台差异归 pal 后端）
 * 
 * 算法说明：
 *   - 填充判定：dx²+dy² ≤ r²（点在圆内）
 *   - Bresenham算法：利用对称性，只计算1/8圆弧，其他7个象限对称映射
 *   - 决策变量d：d>0时y减1，d<=0时y不变，避免浮点运算
 * 
 * @return 0=绘制成功, -1=绘制失败（元素未激活或内存分配失败）
 */
int CircleElement::draw() {
    if (!active || !backend_) return -1;
    
    // 计算画布尺寸和缓冲区大小
    int size = radius * 2;  // 画布尺寸=直径
    int pixels = size * size;  // 总像素数
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
    
    // 步骤1：填充圆形内部（如果启用填充）
    if (should_fill) {
        for (int py = 0; py < size; py++) {
            for (int px = 0; px < size; px++) {
                // 计算像素相对于圆心的距离
                int dx = px - radius;
                int dy = py - radius;
                // 判定是否在圆内：dx²+dy² ≤ r²
                if (dx * dx + dy * dy <= radius * radius) {
                    int pixel_idx = py * size + px;
                    int byte_idx = pixel_idx / 2;
                    int bit_shift = (pixel_idx % 2 == 0) ? 4 : 0;
                    dot_buf[byte_idx] = (dot_buf[byte_idx] & ~(0x0F << bit_shift)) | (fill_idx << bit_shift);
                }
            }
        }
    }
    
    // 步骤2：使用Bresenham算法绘制圆形边框
    int cx = radius, cy = radius;  // 圆心在画布中的坐标
    int x = 0, y = radius;  // 起始点（圆的最右侧）
    int d = 3 - 2 * radius;  // 决策变量初始值
    
    // 迭代绘制1/8圆弧，其他7个象限对称映射
    while (y >= x) {
        // 利用圆形的8向对称性，一次计算8个点
        int points[][2] = {
            {cx + x, cy + y}, {cx - x, cy + y},  // 上、下
            {cx + x, cy - y}, {cx - x, cy - y},  // 上、下
            {cx + y, cy + x}, {cx - y, cy + x},  // 左、右
            {cx + y, cy - x}, {cx - y, cy - x}   // 左、右
        };
        
        // 绘制8个对称点
        for (int i = 0; i < 8; i++) {
            int px = points[i][0];
            int py = points[i][1];
            // 检查点是否在画布范围内
            if (px >= 0 && px < size && py >= 0 && py < size) {
                int pixel_idx = py * size + px;
                int byte_idx = pixel_idx / 2;
                int bit_shift = (pixel_idx % 2 == 0) ? 4 : 0;
                // 设置像素颜色为边框颜色
                dot_buf[byte_idx] = (dot_buf[byte_idx] & ~(0x0F << bit_shift)) | (color_idx << bit_shift);
            }
        }
        
        // 更新决策变量和坐标
        x++;  // x始终递增
        if (d > 0) {
            y--;  // d>0时y递减
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;  // d<=0时y不变
        }
    }
    
    // 经后端将像素缓冲区绘制到画布（后端只读缓冲，绘制完即可释放）
    int ret = backend_->DrawBitmap(channel, rect_id, size, size, dot_buf, buf_len);
    free(dot_buf);  // 释放临时缓冲区
    
    return ret;
}

} // namespace osd
