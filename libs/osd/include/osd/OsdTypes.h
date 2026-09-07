/**
 * OsdTypes.h - OSD类型定义
 * 
 * 功能：
 *   - 定义类型安全的枚举和结构体
 *   - 提供编译期常量 constexpr
 *   - 替代C版本的弱类型 wm_color_t 等
 * 
 * 设计原则：
 *   - 使用 enum class 提供类型安全
 *   - 使用 constexpr 编译期求值，零运行时开销
 *   - 适配嵌入式环境 (C++14, 无C++17特性)
 */

#ifndef OSD_TYPES_H
#define OSD_TYPES_H

#include <stdint.h>
#include <string>
#include <functional>
#include <vector>

namespace osd {

/**
 * OSD元素位置枚举
 */
enum class Position : uint8_t {
    LeftTop = 0,       ///< 左上角
    RightTop = 1,      ///< 右上角
    LeftBottom = 2,    ///< 左下角
    RightBottom = 3,   ///< 右下角
    Custom = 4         ///< 自定义位置
};

/**
 * 日期格式枚举
 */
enum class DateFormat : uint8_t {
    YYYYMMDD = 0,      ///< YYYY-MM-DD HH:MM:SS
    MMDDYYYY = 1,      ///< MM-DD-YYYY HH:MM:SS
    Chinese = 2        ///< YYYY年MM月DD日 HH:MM:SS
};

/**
 * OSD元素类型枚举
 */
enum class ElementType : uint8_t {
    Time = 0,          ///< 时间水印
    Text = 1,          ///< 文本
    Rect = 2,          ///< 矩形
    Circle = 3,        ///< 圆形
    Ellipse = 4,       ///< 椭圆
    Polygon = 5,       ///< 任意多边形（软件扫描线填充，硬件只认位图）
    Bitmap = 6         ///< 位图（BMP 图片加载后量化成调色板索引色）
};

/**
 * 日志级别枚举
 * 
 * 用于日志回调函数中标识日志级别。
 * 调用方可根据级别将日志路由到不同的输出目标(如spdlog)。
 */
enum class LogLevel {
    Debug = 0,  ///< 调试信息,详细执行流程
    Info = 1,   ///< 一般信息,正常操作记录
    Warn = 2,   ///< 警告信息,潜在问题但不影响运行
    Error = 3   ///< 错误信息,操作失败或异常
};

/**
 * 颜色结构体 (ABGR8888 格式)
 * 
 * 注意：硬件OSD使用ABGR字节序，与常规RGBA不同
 */
struct Color {
    uint8_t alpha;     ///< 透明度 (0=透明, 255=不透明)
    uint8_t red;       ///< 红色通道
    uint8_t green;     ///< 绿色通道
    uint8_t blue;      ///< 蓝色通道
    
    /**
     * 构造函数
     */
    constexpr Color(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
        : alpha(a), red(r), green(g), blue(b) {}
    
    /**
     * 默认构造函数 (白色)
     */
    constexpr Color() : alpha(255), red(255), green(255), blue(255) {}
    
    /**
     * 便捷静态方法：白色
     */
    static constexpr Color White() { return {255, 255, 255, 255}; }
    
    /**
     * 便捷静态方法：黑色
     */
    static constexpr Color Black() { return {255, 0, 0, 0}; }
    
    /**
     * 便捷静态方法：红色
     */
    static constexpr Color Red() { return {255, 255, 0, 0}; }
    
    /**
     * 便捷静态方法：绿色
     */
    static constexpr Color Green() { return {255, 0, 255, 0}; }
    
    /**
     * 便捷静态方法：蓝色
     */
    static constexpr Color Blue() { return {255, 0, 0, 255}; }
    
    /**
     * 便捷静态方法：黄色
     */
    static constexpr Color Yellow() { return {255, 255, 255, 0}; }
    
    /**
     * 便捷静态方法：天蓝色 (常用于OSD)
     */
    static constexpr Color SkyBlue() { return {255, 160, 130, 44}; }
    
    /**
     * 判断是否透明
     */
    constexpr bool isTransparent() const { return alpha == 0; }
    
    /**
     * 判断是否不透明
     */
    constexpr bool isOpaque() const { return alpha > 0; }
};

/**
 * SDK硬件调色板 (16色, ABGR8888格式)
 * 
 * 注意：字节序为ABGR，0xAABBGGRR -> R在最低字节
 * SDK初始化时使用此表，颜色匹配算法也基于此表
 */
constexpr uint32_t HARDWARE_PALETTE[16] = {
    0x00000000,  // 0: 透明
    0xffffffff,  // 1: 白色 (R=255,G=255,B=255)
    0xff000000,  // 2: 黑色 (R=0,G=0,B=0)
    0xff0000ff,  // 3: 红色 (R=255,G=0,B=0)
    0xff00ff00,  // 4: 绿色 (R=0,G=255,B=0)
    0xffff0000,  // 5: 蓝色 (R=0,G=0,B=255)
    0xffffff00,  // 6: 黄色 (R=255,G=255,B=0)
    0xffff00ff,  // 7: 青色 (R=255,G=0,B=255)
    0xff00ffff,  // 8: 洋红 (R=0,G=255,B=255)
    0xff786085,  // 9: 特殊色1
    0xff2c8aa0,  // 10: 天蓝色
    0xff68d535,  // 11: 特殊色2
    0xff34aa5a,  // 12: 特殊色3
    0xff43e9ab,  // 13: 特殊色4
    0xff4b55a5,  // 14: 特殊色5
    0xff008080   // 15: 深青色
};

/**
 * 矩形配置
 */
struct RectConfig {
    int x = 0;             ///< X坐标
    int y = 0;             ///< Y坐标
    int width = 0;         ///< 宽度
    int height = 0;        ///< 高度
    int border_width = 1;  ///< 边框宽度
    Color border_color;    ///< 边框颜色
    bool filled = false;   ///< 是否填充
    Color fill_color;      ///< 填充颜色
    int alpha = 20;        ///< 透明度 0~100：0=不透明全显，100=完全看不见；
                           ///< 矩形默认 20（遮盖块半透，量纲按 SDK 注释 0~100）
    
    /**
     * Builder模式：设置位置
     */
    RectConfig& at(int px, int py) { x = px; y = py; return *this; }
    
    /**
     * Builder模式：设置尺寸
     */
    RectConfig& withSize(int w, int h) { width = w; height = h; return *this; }
    
    /**
     * Builder模式：设置边框
     */
    RectConfig& borderWidth(int w) { border_width = w; return *this; }
    
    /**
     * Builder模式：设置边框颜色
     */
    RectConfig& borderColor(Color c) { border_color = c; return *this; }
    
    /**
     * Builder模式：启用填充
     */
    RectConfig& withFill(Color c) { filled = true; fill_color = c; return *this; }
    
    /**
     * Builder模式：设置透明度（0~100，超范围由调用方钳位）
     */
    RectConfig& withOpacity(int a) { alpha = a; return *this; }
};

/**
 * 圆形配置
 */
struct CircleConfig {
    int center_x = 0;      ///< 圆心X坐标
    int center_y = 0;      ///< 圆心Y坐标
    int radius = 10;       ///< 半径
    int border_width = 1;  ///< 边框宽度
    Color border_color;    ///< 边框颜色
    bool filled = false;   ///< 是否填充
    Color fill_color;      ///< 填充颜色
    int alpha = 0;         ///< 透明度 0~100（0=不透明全显）
    
    /**
     * Builder模式：设置圆心
     */
    CircleConfig& at(int cx, int cy) { center_x = cx; center_y = cy; return *this; }
    
    /**
     * Builder模式：设置半径
     */
    CircleConfig& withRadius(int r) { radius = r; return *this; }
    
    /**
     * Builder模式：设置边框
     */
    CircleConfig& borderWidth(int w) { border_width = w; return *this; }
    
    /**
     * Builder模式：设置边框颜色
     */
    CircleConfig& borderColor(Color c) { border_color = c; return *this; }
    
    /**
     * Builder模式：启用填充
     */
    CircleConfig& withFill(Color c) { filled = true; fill_color = c; return *this; }
    
    /**
     * Builder模式：设置透明度（0~100，圆默认 0 不透明）
     */
    CircleConfig& withOpacity(int a) { alpha = a; return *this; }
};

/**
 * 椭圆配置
 */
struct EllipseConfig {
    int center_x = 0;      ///< 椭圆中心X坐标
    int center_y = 0;      ///< 椭圆中心Y坐标
    int rx = 10;           ///< 水平半径
    int ry = 10;           ///< 垂直半径
    int border_width = 1;  ///< 边框宽度
    Color border_color;    ///< 边框颜色
    bool filled = false;   ///< 是否填充
    Color fill_color;      ///< 填充颜色
    int alpha = 0;         ///< 透明度 0~100（0=不透明全显）
    
    /**
     * Builder模式：设置中心
     */
    EllipseConfig& at(int cx, int cy) { center_x = cx; center_y = cy; return *this; }
    
    /**
     * Builder模式：设置半径
     */
    EllipseConfig& withRadius(int rx_, int ry_) { rx = rx_; ry = ry_; return *this; }
    
    /**
     * Builder模式：设置边框
     */
    EllipseConfig& borderWidth(int w) { border_width = w; return *this; }
    
    /**
     * Builder模式：设置边框颜色
     */
    EllipseConfig& borderColor(Color c) { border_color = c; return *this; }
    
    /**
     * Builder模式：启用填充
     */
    EllipseConfig& withFill(Color c) { filled = true; fill_color = c; return *this; }
    
    /**
     * Builder模式：设置透明度（0~100，椭圆默认 0 不透明）
     */
    EllipseConfig& withOpacity(int a) { alpha = a; return *this; }
};

/**
 * 顶点（像素坐标）
 */
struct Point {
    int x = 0;
    int y = 0;
};

/**
 * 多边形配置（任意边数，凹多边形也支持，扫描线奇偶规则填充）
 */
struct PolygonConfig {
    std::vector<Point> points;   ///< 顶点列表（像素绝对坐标，至少 3 个，首尾不用重复）
    Color fill_color;            ///< 填充颜色
    int alpha = 20;              ///< 透明度 0~100：多边形和矩形遮盖同语义，默认 20 半透

    /**
     * Builder模式：设置顶点列表（拷贝进来，配置随后可释放）
     */
    PolygonConfig& withPoints(const std::vector<Point>& pts) { points = pts; return *this; }

    /**
     * Builder模式：启用填充（多边形目前只支持填充，无边框画法）
     */
    PolygonConfig& withFill(Color c) { fill_color = c; return *this; }

    /**
     * Builder模式：设置透明度（0~100）
     */
    PolygonConfig& withOpacity(int a) { alpha = a; return *this; }
};

/**
 * 位图配置（设备本地 BMP 文件，仅支持 24 位无压缩）
 */
struct BitmapConfig {
    std::string image_path;   ///< BMP 文件路径（设备上的绝对路径）
    int x = 0;                ///< 画布左上角X
    int y = 0;                ///< 画布左上角Y
    int width = 0;            ///< 目标显示宽（图片按最近邻采样缩放到这个尺寸）
    int height = 0;           ///< 目标显示高（填 0 由调用方保证，库内会拒绝）
    int alpha = 0;            ///< 透明度 0~100（位图默认 0：logo 要不透明全显）

    /**
     * Builder模式：指定图片文件
     */
    BitmapConfig& fromFile(const std::string& path) { image_path = path; return *this; }

    /**
     * Builder模式：设置位置
     */
    BitmapConfig& at(int px, int py) { x = px; y = py; return *this; }

    /**
     * Builder模式：设置目标显示尺寸（图片大于此尺寸等比缩不裁剪，小于则放大采样）
     */
    BitmapConfig& withSize(int w, int h) { width = w; height = h; return *this; }

    /**
     * Builder模式：设置透明度（0~100）
     */
    BitmapConfig& withOpacity(int a) { alpha = a; return *this; }
};

/**
 * OSD管理器配置
 */
struct ManagerConfig {
    int video_width = 1920;           ///< 视频宽度
    int video_height = 1080;          ///< 视频高度
    int channel = 0;                  ///< 视频通道 (0=主通道, 1=子通道)
    std::string font_file;            ///< 字体文件路径
    int font_size = 32;               ///< 字体大小
    
    // ========== 日志配置 ==========
    /**
     * 日志回调函数类型
     *
     * 调用方可在此函数中将日志输出到 spdlog/syslog/控制台等。
     *
     * @param level 日志级别（Debug/Info/Warn/Error）
     * @param msg 日志消息内容（不包含级别前缀）
     */
    using LogCallback = std::function<void(LogLevel level, const std::string& msg)>;
    LogCallback logCallback;          ///< 日志回调，不设置则丢弃内部日志
    
    /**
     * Builder模式：设置视频分辨率
     */
    ManagerConfig& withResolution(int w, int h) { video_width = w; video_height = h; return *this; }
    
    /**
     * Builder模式：设置通道
     */
    ManagerConfig& withChannel(int ch) { channel = ch; return *this; }
    
    /**
     * Builder模式：设置字体文件
     */
    ManagerConfig& withFont(const std::string& path) { font_file = path; return *this; }
    
    /**
     * Builder模式：设置字体大小
     */
    ManagerConfig& withFontSize(int size) { font_size = size; return *this; }
    
    /**
     * Builder模式：设置日志回调
     */
    ManagerConfig& withLogCallback(LogCallback cb) { logCallback = std::move(cb); return *this; }
};

/**
 * 时间元素配置
 */
struct TimeElementConfig {
    int x = 0;                       ///< X坐标
    int y = 0;                       ///< Y坐标
    int width = 0;                  ///< 画布宽度
    int height = 0;                  ///< 画布高度
    int font_size = 10;               ///< 字体大小
    Color font_color;                 ///< 字体颜色
    Color bg_color = Color(0, 0, 0, 0);  ///< 背景颜色（默认透明，alpha=0）
    DateFormat date_format = DateFormat::YYYYMMDD;  ///< 日期格式
    bool show_week = false;           ///< 是否显示星期
    std::string custom_format;        ///< 自定义strftime格式 (空则使用date_format)
    Position position_type = Position::Custom;  ///< 位置类型
    
    /**
     * Builder模式：设置位置
     */
    TimeElementConfig& at(int px, int py) { x = px; y = py; return *this; }
    
    /**
     * Builder模式：设置尺寸
     */
    TimeElementConfig& withSize(int w, int h) { width = w; height = h; return *this; }
    
    /**
     * Builder模式：设置颜色
     */
    TimeElementConfig& withColor(Color c) { font_color = c; return *this; }
    
    /**
     * Builder模式：设置字体大小
     */
    TimeElementConfig& withFontSize(int size) { font_size = size; return *this; }
    
    /**
     * Builder模式：设置背景颜色
     */
    TimeElementConfig& withBgColor(Color c) { bg_color = c; return *this; }
    
    /**
     * Builder模式：设置自定义格式
     */
    TimeElementConfig& customFormat(const std::string& fmt) { custom_format = fmt; return *this; }
    
    /**
     * Builder模式：设置日期格式
     */
    TimeElementConfig& dateFormat(DateFormat fmt) { date_format = fmt; return *this; }
    
    /**
     * Builder模式：设置显示星期
     */
    TimeElementConfig& showWeek(bool show) { show_week = show; return *this; }
    
    /**
     * Builder模式：设置位置类型
     */
    TimeElementConfig& positionType(Position pos) { position_type = pos; return *this; }
};

/**
 * 文本元素配置
 */
struct TextElementConfig {
    std::string text;                 ///< 文本内容
    int x = 10;                       ///< X坐标
    int y = 10;                       ///< Y坐标
    int font_size = 32;               ///< 字体大小
    Color color;                      ///< 字体颜色
    Color bg_color = Color(0, 0, 0, 0);  ///< 背景颜色（默认透明，alpha=0）
    
    /**
     * Builder模式：设置文本
     */
    TextElementConfig& withText(const std::string& t) { text = t; return *this; }
    
    /**
     * Builder模式：设置位置
     */
    TextElementConfig& at(int px, int py) { x = px; y = py; return *this; }
    
    /**
     * Builder模式：设置字体大小
     */
    TextElementConfig& withFontSize(int size) { font_size = size; return *this; }
    
    /**
     * Builder模式：设置颜色
     */
    TextElementConfig& withColor(Color c) { color = c; return *this; }
    
    /**
     * Builder模式：设置背景颜色
     */
    TextElementConfig& withBgColor(Color c) { bg_color = c; return *this; }
};
} // namespace osd

#endif // OSD_TYPES_H
