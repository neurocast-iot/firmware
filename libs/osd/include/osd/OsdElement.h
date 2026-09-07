/**
 * OsdElement.h - OSD元素基类
 * 
 * 功能：
 *   - 定义OSD元素的通用接口和生命周期
 *   - 使用非虚基类设计，避免vtable开销（嵌入式优化）
 *   - 通过type枚举+switch分发，零虚函数开销
 * 
 * 设计原则：
 *   - POD结构 (Plain Old Data)，内存布局紧凑
 *   - 非虚函数，运行时switch分发
 *   - 固定大小缓冲，避免动态分配
 */

#ifndef OSD_ELEMENT_H
#define OSD_ELEMENT_H

#include "OsdTypes.h"
#include "pal/osd_backend.h"
#include <string.h>
#include <vector>

namespace osd {

/**
 * 最大像素缓冲区大小 (512x512 / 2 = 128KB)
 * 用于矩形/圆形/椭圆的像素级绘制
 */
#define MAX_PIXEL_BUFFER_SIZE 131072

/**
 * OSD元素基类 (非虚，嵌入式优化)
 * 
 * 设计说明：
 *   - 不使用虚函数，避免vtable/vptr开销（每元素节省~40字节）
 *   - 使用type枚举标识具体类型
 *   - Manager通过switch(type)调用具体实现
 *   - POD结构，可直接memcpy，便于调试
 */
struct Element {
    ElementType type;          ///< 元素类型
    int rect_id;               ///< SDK区域ID
    bool active;               ///< 是否已激活
    int channel;               ///< 视频通道
    
    // 日志回调（由Manager在创建元素时传递）
    ManagerConfig::LogCallback logCallback_;  ///< 日志回调函数
    
    // 绘制后端（由 OsdEngine 创建时注入，生命周期归引擎管，元素不释放）
    pal::IOsdBackend* backend_ = nullptr;
    
    /**
     * 构造函数
     * @param t 元素类型
     */
    Element(ElementType t)
        : type(t), rect_id(-1), active(false), channel(0) {}
    
    /**
     * 虚拟析构函数（防止子类泄漏）
     * 注意：虽然是虚函数，但只在销毁时调用一次，不影响运行时性能
     */
    virtual ~Element() = default;
    
    /**
     * 设置日志回调（由Manager调用）
     * 
     * @param cb 日志回调函数
     */
    void setLogCallback(ManagerConfig::LogCallback cb) {
        logCallback_ = std::move(cb);
    }
    
    /**
     * 内部日志方法
     * 
     * 功能说明：
     *   - 如果配置了logCallback_，调用回调输出日志
     *   - 如果未配置logCallback_，丢弃日志（零开销）
     * 
     * @param level 日志级别
     * @param msg 日志消息内容
     */
    void log(LogLevel level, const std::string& msg) {
        if (logCallback_) {
            logCallback_(level, msg);
        }
    }
    
    /**
     * 设置绘制后端（由 OsdEngine 工厂在 create 前调用）
     * 
     * @param b 后端指针，须比元素活得久（引擎持有，天然满足）
     */
    void setBackend(pal::IOsdBackend* b) {
        backend_ = b;
    }
    
    /**
     * 创建元素（经后端建画布）
     * @param ch 视频通道
     * @param rid 区域ID
     * @param videoWidth 通道实际分辨率宽（引擎传入，画布钳位基准）
     * @param videoHeight 通道实际分辨率高
     * @return 0成功，-1失败
     */
    virtual int create(int ch, int rid, int videoWidth, int videoHeight) = 0;
    
    /**
     * 绘制元素
     * @return 0成功，-1失败
     */
    virtual int draw() = 0;
    
    /**
     * 更新元素（仅TimeElement需要实现）
     * @return 0成功或无需更新，-1失败
     */
    virtual int update() { return 0; }
    
    /**
     * 销毁元素（经后端销毁画布）
     */
    virtual void destroy();
    
    /**
     * 获取区域ID
     */
    int getRectId() const { return rect_id; }
    
    /**
     * 判断是否激活
     */
    bool isActive() const { return active; }
    
    /**
     * 获取元素类型
     */
    ElementType getType() const { return type; }
};

/**
 * 时间元素
 * 
 * 设计说明：
 *   - TimeElement 本身不包含刷新线程
 *   - 刷新由 OsdManager 集中管理（统一线程调度）
 *   - 外部只需调用 create() 和 destroy()
 */
struct TimeElement : public Element {
    int x, y;                      ///< 位置坐标
    int width, height;             ///< 画布尺寸
    int font_size;                 ///< 字体大小
    Color font_color;              ///< 字体颜色
    Color bg_color;                ///< 背景颜色（默认透明）
    DateFormat date_format;        ///< 日期格式
    bool show_week;                ///< 是否显示星期
    std::string custom_format;     ///< 自定义格式
    Position position_type;        ///< 位置类型
    
    char time_buffer_[64];         ///< 时间字符串缓冲
    std::string timeText;          ///< 外部传入的时间字符串
    
    /**
     * 构造函数
     */
    TimeElement();
    
    /**
     * 从配置构造
     */
    explicit TimeElement(const TimeElementConfig& config);
    
    // 实现Element接口
    virtual int create(int ch, int rid, int videoWidth, int videoHeight) override;
    virtual int draw() override;
    virtual int update() override;  // OsdManager 定时调用此方法
    
    /**
     * 设置时间文本（由 OsdManager 调用）
     * 
     * @param text 时间字符串（如 "2026-06-11 23:33:45"）
     */
    void setTimeText(const std::string& text);
    
private:
    /**
     * 生成时间字符串（已废弃，保留用于首次初始化）
     */
    void generateTimeString();
};

/**
 * 文本元素
 */
struct TextElement : public Element {
    int x, y;                      ///< 位置坐标
    int font_size;                 ///< 字体大小
    Color color;                   ///< 字体颜色
    Color bg_color;                ///< 背景颜色（默认透明）
    std::string text;              ///< 文本内容
    
    /**
     * 构造函数
     */
    TextElement();
    
    /**
     * 从配置构造
     */
    explicit TextElement(const TextElementConfig& config);
    
    // 实现Element接口
    virtual int create(int ch, int rid, int videoWidth, int videoHeight) override;
    virtual int draw() override;
    
    /**
     * 更新文本内容
     */
    void setText(const std::string& new_text);
    
private:
    /**
     * 计算文本宽度
     */
    int calculateWidth() const;
};

/**
 * 矩形元素
 */
struct RectElement : public Element {
    int x, y;                      ///< 位置坐标
    int width, height;             ///< 尺寸
    int border_width;              ///< 边框宽度
    Color border_color;            ///< 边框颜色
    bool filled;                   ///< 是否填充
    Color fill_color;              ///< 填充颜色
    int alpha;                     ///< 透明度 0~100（建画布时交给后端）
    
    /**
     * 构造函数
     */
    RectElement();
    
    /**
     * 从配置构造
     */
    explicit RectElement(const RectConfig& config);
    
    // 实现Element接口
    virtual int create(int ch, int rid, int videoWidth, int videoHeight) override;
    virtual int draw() override;
};

/**
 * 圆形元素
 */
struct CircleElement : public Element {
    int center_x, center_y;        ///< 圆心坐标
    int radius;                    ///< 半径
    int border_width;              ///< 边框宽度
    Color border_color;            ///< 边框颜色
    bool filled;                   ///< 是否填充
    Color fill_color;              ///< 填充颜色
    int alpha;                     ///< 透明度 0~100（建画布时交给后端）
    
    /**
     * 构造函数
     */
    CircleElement();
    
    /**
     * 从配置构造
     */
    explicit CircleElement(const CircleConfig& config);
    
    // 实现Element接口
    virtual int create(int ch, int rid, int videoWidth, int videoHeight) override;
    virtual int draw() override;
};

/**
 * 椭圆元素
 */
struct EllipseElement : public Element {
    int center_x, center_y;        ///< 椭圆中心坐标
    int rx, ry;                    ///< 水平/垂直半径
    int border_width;              ///< 边框宽度
    Color border_color;            ///< 边框颜色
    bool filled;                   ///< 是否填充
    Color fill_color;              ///< 填充颜色
    int alpha;                     ///< 透明度 0~100（建画布时交给后端）
    
    /**
     * 构造函数
     */
    EllipseElement();
    
    /**
     * 从配置构造
     */
    explicit EllipseElement(const EllipseConfig& config);
    
    // 实现Element接口
    virtual int create(int ch, int rid, int videoWidth, int videoHeight) override;
    virtual int draw() override;
};

/**
 * 多边形元素（任意边数，凹多边形也支持）
 *
 * 实现思路：硬件叠加器只认索引色位图，不知道什么叫多边形，
 * 所以库内把顶点扫描线填充成 4bpp 点阵再走 DrawBitmap，
 * 跟圆/椭圆同一条路。顶点存像素绝对坐标，create 时算出包围盒当画布。
 */
struct PolygonElement : public Element {
    std::vector<Point> points;     ///< 顶点列表（像素绝对坐标，至少 3 个）
    Color fill_color;              ///< 填充颜色
    int alpha = 20;                ///< 透明度 0~100（默认和矩形遮盖同语义）

    /* create 时钳位后的画布位置和尺寸，draw 按它做顶点平移和裁剪 */
    int x_ = 0, y_ = 0;
    int canvas_w_ = 0, canvas_h_ = 0;
    
    /**
     * 构造函数
     */
    PolygonElement();
    
    /**
     * 从配置构造（顶点拷贝进来）
     */
    explicit PolygonElement(const PolygonConfig& config);
    
    // 实现Element接口（create 算包围盒建画布，draw 扫描线填充）
    virtual int create(int ch, int rid, int videoWidth, int videoHeight) override;
    virtual int draw() override;
};

/**
 * 位图元素（设备本地 BMP 图片贴上屏）
 *
 * 实现思路：读 24 位无压缩 BMP（不引任何图像库，手写解析头 + 像素），
 * 每像素到硬件 16 色调色板就近色匹配，最近邻采样缩到目标尺寸，
 * 填成 4bpp 点阵走 DrawBitmap。静态元素只在创建时画一次，
 * 解码/量化开销不进刷新线程。
 */
struct BitmapElement : public Element {
    std::string image_path;        ///< BMP 文件路径
    int x, y;                      ///< 画布位置（像素）
    int width, height;             ///< 目标显示尺寸（图片采样缩放到这个尺寸）
    int alpha = 0;                 ///< 透明度 0~100（默认 0 不透明）
    
    /**
     * 构造函数
     */
    BitmapElement();
    
    /**
     * 从配置构造（文件不在这时读，draw 时才加载）
     */
    explicit BitmapElement(const BitmapConfig& config);
    
    // 实现Element接口（create 建画布，draw 读文件+量化+贴画布）
    virtual int create(int ch, int rid, int videoWidth, int videoHeight) override;
    virtual int draw() override;
};

} // namespace osd

#endif // OSD_ELEMENT_H
