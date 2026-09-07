/**
 * TimeElement.cpp - 时间元素实现
 * 
 * 功能：
 *   - 绘制时间水印（支持自定义strftime格式）
 *   - update()方法自动刷新时间
 */

#include "OsdElement.h"
#include "OsdUtils.h"

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

namespace osd {

/**
 * 默认构造函数
 * 
 * 功能说明：
 *   - 初始化时间元素为默认状态（白色字体、YYYY-MM-DD格式、不显示星期）
 *   - 设置默认位置和尺寸（10,10,400x60）
 *   - 清空时间字符串缓冲区
 */
TimeElement::TimeElement()
    : Element(ElementType::Time)
    , x(10), y(10), width(400), height(60)
    , font_size(32)
    , font_color(Color::White())
    , bg_color(Color(0, 0, 0, 0))  // 默认透明背景
    , date_format(DateFormat::YYYYMMDD)
    , show_week(false)
    , position_type(Position::Custom)
{
    memset(time_buffer_, 0, sizeof(time_buffer_));
}

/**
 * 带配置的构造函数
 * 
 * 功能说明：
 *   - 根据配置参数初始化时间元素的所有属性
 *   - 支持自定义时间格式（custom_format）
 *   - 生成初始时间字符串用于首次绘制
 * 
 * @param config 时间元素配置（位置、尺寸、颜色、日期格式等）
 */
TimeElement::TimeElement(const TimeElementConfig& config)
    : Element(ElementType::Time)
    , x(config.x), y(config.y)
    , width(config.width), height(config.height)
    , font_size(config.font_size)
    , font_color(config.font_color)
    , bg_color(config.bg_color)
    , date_format(config.date_format)
    , show_week(config.show_week)
    , custom_format(config.custom_format)
    , position_type(config.position_type)
{
    memset(time_buffer_, 0, sizeof(time_buffer_));
    
    // 生成初始时间字符串（用于首次绘制）
    //generateTimeString();
}

/**
 * 创建OSD画布
 * 
 * 功能说明：
 *   - 调用clampToBounds()限制画布尺寸，防止越界（分辨率由引擎传入）
 *   - 组装pal::OsdCanvas经后端创建画布（平台差异归 pal 后端）
 *   - 创建成功后立即绘制初始内容
 * 
 * @param ch 视频通道号（0=主通道, 1=子通道）
 * @param rid OSD区域ID（0-3，硬件限制最多4个区域）
 * @param video_w 通道实际分辨率宽（引擎传入，未设置时已在引擎侧兜底）
 * @param video_h 通道实际分辨率高
 * @return 0=创建成功, -1=创建失败
 */
int TimeElement::create(int ch, int rid, int video_w, int video_h) {
    channel = ch;
    rect_id = rid;
    
    // 限制画布尺寸，防止超出视频帧范围
    CanvasBounds bounds = clampToBounds(x, y, width, height, video_w, video_h, video_w, video_h);
    
    // 配置OSD画布属性（平台无关描述，后端负责映射到硬件结构体）
    pal::OsdCanvas canvas;
    canvas.channel = ch;          // 视频通道号
    canvas.region_id = rid;       // OSD区域ID
    canvas.x = bounds.x;          // 画布左上角X坐标
    canvas.y = bounds.y;          // 画布左上角Y坐标
    canvas.width = bounds.width;   // 画布宽度
    canvas.height = bounds.height; // 画布高度
    
    // 设置背景颜色（如果bg_color.alpha=0则透明）
    log(LogLevel::Info, "TimeElement::create - bg_color=(" + 
        std::to_string(bg_color.alpha) + "," +
        std::to_string(bg_color.red) + "," +
        std::to_string(bg_color.green) + "," +
        std::to_string(bg_color.blue) + ")");
    
    if (bg_color.alpha == 0) {
        canvas.bg_color_index = 0;  // 透明背景
        log(LogLevel::Info, "TimeElement::create - using TRANSPARENT background");
    } else {
        canvas.bg_color_index = matchColorIndex(bg_color);  // 使用配置的背景色
        log(LogLevel::Info, "TimeElement::create - using palette index " + 
            std::to_string(canvas.bg_color_index));
    }
    canvas.alpha = 0;  // 透明度
    
    // 经后端创建画布（硬件/软渲染由平台后端决定）
    if (!backend_ || !backend_->CreateCanvas(canvas)) {
        log(LogLevel::Error, "Failed to create time canvas, ch=" + std::to_string(ch) + ", rect=" + std::to_string(rid));
        return -1;
    }
    
    active = true;
    
    // 立即绘制初始时间
    return draw();
}

/**
 * 设置时间文本（由 OsdManager 调用）
 * 
 * @param text 时间字符串（如 "2026-06-11 23:33:45"）
 */
void TimeElement::setTimeText(const std::string& text) {
    timeText = text;
    // 同步到 time_buffer_（用于 draw() 绘制）
    strncpy(time_buffer_, text.c_str(), 63);
    time_buffer_[63] = '\0';
}

/**
 * 绘制时间水印
 * 
 * 功能说明：
 *   - 使用已设置的时间字符串（由 OsdManager 传入）
 *   - 调用convertToWideChar()将UTF-8/GBK字符串转换为宽字符（UTF-16）
 *   - 经后端在画布上绘制时间文本（字体文件刷新归后端内部处理）
 * 
 * @return 0=绘制成功, -1=绘制失败（元素未激活或后端调用失败）
 */
int TimeElement::draw() {
    if (!active || !backend_) {
        return -1;
    }
    
    // 检查时间字符串是否为空
    if (time_buffer_[0] == '\0') {
        log(LogLevel::Warn, "TimeElement::draw - time_buffer is empty, skip drawing");
        return 0;  // 返回0表示跳过，不是错误
    }
    
    // 设置当前文本的字体大小（动态配置）
    backend_->SetFontSize(channel, font_size);
    
    // 设置字体颜色（前景=字体颜色，背景=透明）
    backend_->SetDrawColor(matchColorIndex(font_color), 0);
    
    // 时间字符串由 OsdManager 通过 setTimeText() 传入，此处不读时钟
    
    // 按 '\n' 分割字符串为多行
    std::vector<std::string> lines;
    std::string current_line;
    for (int i = 0; time_buffer_[i] != '\0'; i++) {
        if (time_buffer_[i] == '\n') {
            lines.push_back(current_line);
            current_line.clear();
        } else {
            current_line += time_buffer_[i];
        }
    }
    if (!current_line.empty()) {
        lines.push_back(current_line);
    }
    
    // 循环绘制每一行（行内转宽字符后经后端绘制，字体刷新归后端）
    for (size_t i = 0; i < lines.size(); i++) {
        uint16_t osd_str[100];
        memset(osd_str, 0, sizeof(osd_str));
        int osd_len = convertToWideChar(osd_str, lines[i].c_str(), 99);
        
        // 计算当前行的 yoffset（行间距固定 4 像素，与原实现一致）
        int yoffset = i * (font_size + 4);
        
        int ret = backend_->DrawWideStr(channel, rect_id, 0, yoffset, osd_str, osd_len);
        if (ret < 0) {
            log(LogLevel::Error, "Failed to draw line " + std::to_string(i) + ", ret=" + std::to_string(ret));
            return -1;
        }
    }
    
    return 0;
}

/**
 * 更新时间水印
 * 
 * 功能说明：
 *   - 检查元素是否激活和启用
 *   - 如果已启用，调用draw()重新绘制最新时间
 *   - 此方法应由主循环定期调用（如每秒一次）
 * 
 * @return 0=更新成功, -1=更新失败
 */
int TimeElement::update() {
    if (!active) {
        return 0;  // 元素未激活，无需更新
    }
    
    // 重新绘制时间（获取最新系统时间）
    return draw();
}

/**
 * 生成时间字符串
 * 
 * 功能说明：
 *   - 获取当前系统时间（time() + localtime_r()）
 *   - 优先使用自定义格式（custom_format），如果为空则使用预定义格式
 *   - 支持三种预定义格式：YYYY-MM-DD、MM-DD-YYYY、中文格式（年月日）
 *   - 如果show_week=true，在时间字符串后追加星期（如" 周一"）
 *   - 如果strftime失败，使用默认格式"%Y-%m-%d %H:%M:%S"
 * 
 * 注意事项：
 *   - 时间字符串存储在time_buffer_成员变量中（最大64字节）
 *   - 中文字符用 UTF-8 直写（源文件本身是 UTF-8），由
  *     convertToWideChar 统一转 GB2312 交给字库；历史上这里硬编码过
  *     GB2312 字节，与新转换器的 UTF-8 输入约定冲突后改回直写
 */
void TimeElement::generateTimeString() {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    
    size_t len = 0;
    
    // 优先使用自定义格式（如前端传入的"YYYY/MM/DD HH:mm:ss"）
    if (!custom_format.empty()) {
        len = strftime(time_buffer_, sizeof(time_buffer_), custom_format.c_str(), &tm_info);
    } else {
        // 使用预定义格式
        switch (date_format) {
            case DateFormat::YYYYMMDD:
                // 格式：2024-01-15 14:30:25
                len = strftime(time_buffer_, sizeof(time_buffer_), "%Y-%m-%d %H:%M:%S", &tm_info);
                break;
            case DateFormat::MMDDYYYY:
                // 格式：01-15-2024 14:30:25
                len = strftime(time_buffer_, sizeof(time_buffer_), "%m-%d-%Y %H:%M:%S", &tm_info);
                break;
            case DateFormat::Chinese:
                // 格式：2024年01月15日 14:30:25（中文直写，编码归转换器管）
                snprintf(time_buffer_, sizeof(time_buffer_),
                         "%04d年%02d月%02d日 %02d:%02d:%02d",
                         tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                         tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
                len = strlen(time_buffer_);
                break;
        }
    }
    
    // 如果strftime失败（返回0），使用默认格式
    if (len == 0) {
        strftime(time_buffer_, sizeof(time_buffer_), "%Y-%m-%d %H:%M:%S", &tm_info);
    }
    
    // 如果需要显示星期，追加星期字符串（中文直写，编码归转换器管）
    if (show_week) {
        const char* weekdays[] = {
            "周日",
            "周一",
            "周二",
            "周三",
            "周四",
            "周五",
            "周六"
        };
        
        char week_str[16];
        snprintf(week_str, sizeof(week_str), " %s", weekdays[tm_info.tm_wday]);
        strncat(time_buffer_, week_str, sizeof(time_buffer_) - strlen(time_buffer_) - 1);
    }
}

/**
 * 销毁元素基类的画布（五个元素子类共用）
 * 
 * 功能说明：
 *   - 经后端销毁画布（硬件/软渲染由平台后端决定）
 *   - 重置active和rect_id状态
 * 
 * 注意事项：
 *   - 只有active=true时才执行销毁操作（防止重复销毁）
 */
void Element::destroy() {
    if (!active) {
        return;  // 元素未激活，无需销毁
    }
    
    // 经后端销毁画布（后端缺失时只清状态，防御异常构造路径）
    if (backend_) {
        backend_->DestroyCanvas(channel, rect_id);
    }
    active = false;
    rect_id = -1;
}

} // namespace osd
