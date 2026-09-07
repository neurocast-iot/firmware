/**
 * TextElement.cpp - 文本元素实现
 */

#include "OsdElement.h"
#include "OsdUtils.h"

#include <stdio.h>
#include <string.h>

namespace osd {

/**
 * 默认构造函数
 * 
 * 功能说明：
 *   - 初始化文本元素为默认状态（白色字体、32号字体）
 *   - 设置默认位置（10,10）
 */
TextElement::TextElement()
    : Element(ElementType::Text)
    , x(10), y(10), font_size(32)
    , color(Color::White())
    , bg_color(Color(0, 0, 0, 0))  // 默认透明背景
{
}

/**
 * 带配置的构造函数
 * 
 * 功能说明：
 *   - 根据配置参数初始化文本元素的所有属性
 *   - 设置文本内容、位置、字体大小和颜色
 * 
 * @param config 文本元素配置（文本内容、位置、字体大小、颜色等）
 */
TextElement::TextElement(const TextElementConfig& config)
    : Element(ElementType::Text)
    , x(config.x), y(config.y)
    , font_size(config.font_size)
    , color(config.color)
    , bg_color(config.bg_color)
    , text(config.text)
{
}

/**
 * 创建OSD画布
 * 
 * 功能说明：
 *   - 调用calculateWidth()计算文本实际宽度（根据字体大小和字符数）
 *   - 调用clampToBounds()限制画布尺寸，防止越界（分辨率由引擎传入）
 *   - 组装pal::OsdCanvas经后端创建画布，创建成功后立即绘制初始文本
 * 
 * @param ch 视频通道号（0=主通道, 1=子通道）
 * @param rid OSD区域ID（0-3）
 * @param video_w 通道实际分辨率宽（引擎传入）
 * @param video_h 通道实际分辨率高
 * @return 0=创建成功, -1=创建失败
 */
int TextElement::create(int ch, int rid, int video_w, int video_h) {
    channel = ch;
    rect_id = rid;
    
    // 计算文本实际宽度（font_size * 字符数 * 缩放系数）
    int text_width = calculateWidth();
    int text_height = font_size + 8;  // 高度=字体大小+8像素余量
    
    // 限制画布尺寸，防止超出视频帧范围
    CanvasBounds bounds = clampToBounds(x, y, text_width, text_height, video_w, video_h, video_w, video_h);
    
    // 配置OSD画布属性（平台无关描述，后端负责映射到硬件结构体）
    pal::OsdCanvas canvas;
    canvas.channel = ch;
    canvas.region_id = rid;
    canvas.x = bounds.x;
    canvas.y = bounds.y;
    canvas.width = bounds.width;
    canvas.height = bounds.height;
    
    // 设置背景颜色（如果bg_color.alpha=0则透明）
    if (bg_color.alpha == 0) {
        canvas.bg_color_index = 0;  // 透明背景
    } else {
        canvas.bg_color_index = matchColorIndex(bg_color);  // 使用配置的背景色
    }
    canvas.alpha = 0;
    
    // 经后端创建画布（硬件/软渲染由平台后端决定）
    if (!backend_ || !backend_->CreateCanvas(canvas)) {
        log(LogLevel::Error, "Failed to create text canvas");
        return -1;
    }
    
    active = true;
    
    // 立即绘制初始文本
    return draw();
}

/**
 * 绘制文本
 * 
 * 功能说明：
 *   - 检查元素是否激活和文本是否为空
 *   - 调用convertToWideChar()将文本转换为宽字符（UTF-16）
 *   - 经后端在画布上绘制文本（字体文件刷新归后端内部处理）
 * 
 * @return 0=绘制成功, -1=绘制失败（元素未激活、文本为空或后端调用失败）
 */
int TextElement::draw() {
    if (!active || !backend_ || text.empty()) {
        return -1;
    }
    
    // 设置当前文本的字体大小（动态配置）
    backend_->SetFontSize(channel, font_size);
    
    // 设置字体颜色（前景=字体颜色，背景=透明）
    backend_->SetDrawColor(matchColorIndex(color), 0);
    
    // 转换为宽字符（UTF-8/GBK -> UTF-16，后端需要宽字符）
    uint16_t osd_str[100];
    memset(osd_str, 0, sizeof(osd_str));
    int osd_len = convertToWideChar(osd_str, text.c_str(), 99);
    
    // 经后端在画布上绘制文本字符串（字体刷新由后端在画前自动处理）
    if (backend_->DrawWideStr(channel, rect_id, 0, 0, osd_str, osd_len) < 0) {
        log(LogLevel::Error, "Failed to draw text");
        return -1;
    }
    
    return 0;
}

/**
 * 设置文本内容
 * 
 * 功能说明：
 *   - 更新文本内容并立即重绘（如果元素已激活和启用）
 *   - 用于动态更新OSD文本（如显示设备状态、告警信息等）
 * 
 * @param new_text 新的文本内容
 */
void TextElement::setText(const std::string& new_text) {
    text = new_text;
    
    // 如果元素已激活，立即重绘新文本
    if (active) {
        draw();
    }
}

/**
 * 计算文本宽度
 * 
 * 功能说明：
 *   - 调用calculateTextWidth()工具函数计算文本实际像素宽度
 *   - 计算公式：font_size * 字符数 * 缩放系数（考虑中英文差异）
 * 
 * @return 文本宽度（像素）
 */
int TextElement::calculateWidth() const {
    return calculateTextWidth(text, font_size);
}

} // namespace osd
