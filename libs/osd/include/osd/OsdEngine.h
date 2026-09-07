/**
 * OsdEngine.h - OSD 引擎（实例化的叠加层门面）
 * 
 * 功能：
 *   - 通过 pal::IOsdBackend 驱动平台叠加能力（自身零厂商依赖）
 *   - 管理全局初始化、字体文件、通道分辨率、颜色表
 *   - 提供 Element 创建工厂（创建时把后端指针与通道分辨率注入元素）
 * 
 * 设计说明：
 *   - 由 OsdSdkManager 静态门面重构而来：全局状态（初始化标志、色表、
 *     通道分辨率）全部收进实例，生命周期由持有方（mediad OsdService）管理
 *   - 后端构造注入：ARM 上由 pal::CreateOsdBackend() 得到硬件后端，
 *     x86 单测注入软渲染后端
 *   - 线程安全：引擎自身不加锁，由调用方串行化（OsdService 的 m_mutex）
 */

#ifndef OSD_ENGINE_H
#define OSD_ENGINE_H

#include <memory>
#include <string>
#include "OsdTypes.h"

/* 前向声明，头文件不引 pal 具体类型 */
namespace pal { class IOsdBackend; }

namespace osd {
class Element;
struct TimeElementConfig;
struct TextElementConfig;
struct RectConfig;
struct CircleConfig;
struct EllipseConfig;
struct PolygonConfig;
struct BitmapConfig;

class OsdEngine {
public:
    /**
     * 构造引擎
     * 
     * @param backend 叠加后端（构造注入，引擎独占所有权）
     * @param fontFile 字体文件路径（每次画字符串前由后端重设，见接口注释）
     */
    OsdEngine(std::unique_ptr<pal::IOsdBackend> backend, std::string fontFile);
    
    /* 析构不自动销毁叠加子系统：是否释放由持有方按相机会话时序决定 */
    ~OsdEngine();
    
    OsdEngine(const OsdEngine&) = delete;
    OsdEngine& operator=(const OsdEngine&) = delete;
    
    /**
     * 设置日志回调（元素创建时会透传给元素）
     */
    void setLogCallback(ManagerConfig::LogCallback cb);
    
    /**
     * 更换字体文件路径（配置热更用）
     * 
     * 立即透传给后端记住，下一次画字符串即生效，不需要重建元素
     */
    void setFontFile(const std::string& path);
    
    /**
     * 初始化叠加子系统（幂等：已初始化时直接返回成功）
     * 
     * @return 0=成功, -1=后端初始化失败
     */
    int init();
    
    /**
     * 设置通道视频分辨率（元素建画布时的钳位基准）
     * 
     * @param channel 视频通道号（0/1）
     * @param width 视频宽度
     * @param height 视频高度
     */
    void setChannelResolution(int channel, int width, int height);
    
    /**
     * 获取通道视频分辨率
     * 
     * @return 0=成功, -1=未设置过（调用方用默认值兜底）
     */
    int getChannelResolution(int channel, int& width, int& height);
    
    /**
     * 获取通道最大画布尺寸（问后端，跟通道分辨率绑定）
     */
    int getMaxRectSize(int channel, int& maxWidth, int& maxHeight);

    /**
     * 每通道允许同时存在的元素数量上限（问后端，平台能力而非全局常量：
     * anyka 芯片固定 4，软渲染/其他平台更大）。上层按此截断多余元素。
     *
     * @return 上限值；后端缺失时返回 0（调用方应视为不可用）
     */
    int getMaxRegions();
    
    /**
     * 查询是否已初始化
     */
    bool isInitialized();
    
    /**
     * 提交所有画布到 VI 显示
     */
    int commitToVi();
    
    /**
     * 销毁叠加子系统（相机重启前必须调，绑定的是旧 VI 会话）
     */
    void destroy();
    
    /**
     * 重新绘制元素（统一入口）
     */
    int redrawElement(Element* elem);
    
    // ========== 元素创建方法（内部注入后端 + 通道分辨率） ==========
    
    Element* createTimeElement(const TimeElementConfig& config, int channel, int rect_id);
    Element* createTextElement(const TextElementConfig& config, int channel, int rect_id);
    Element* createRectElement(const RectConfig& config, int channel, int rect_id);
    Element* createCircleElement(const CircleConfig& config, int channel, int rect_id);
    Element* createEllipseElement(const EllipseConfig& config, int channel, int rect_id);
    Element* createPolygonElement(const PolygonConfig& config, int channel, int rect_id);
    Element* createBitmapElement(const BitmapConfig& config, int channel, int rect_id);
    
    /**
     * 销毁元素（销毁画布 + 释放内存）
     */
    void destroyElement(Element* elem);
    
    // ========== 颜色管理方法 ==========
    
    /**
     * 将RGB颜色转换为硬件调色板索引
     */
    int matchColorIndex(int r, int g, int b, int alpha);
    
    /**
     * 设置自定义颜色表（下次 init 生效）
     */
    void setColorTable(const unsigned int* colors);
    
    /**
     * 获取当前颜色表
     */
    const unsigned int* getColorTable();
    
    /**
     * 后端裸指针（元素绘制用；所有权仍在引擎）
     */
    pal::IOsdBackend* backend() { return backend_.get(); }

private:
    std::unique_ptr<pal::IOsdBackend> backend_;   ///< 叠加后端（构造注入）
    std::string fontFile_;                        ///< 字体文件路径
    ManagerConfig::LogCallback logCallback_;      ///< 日志回调
    
    bool initialized_ = false;                    ///< 叠加子系统是否已初始化
    unsigned int colorTables_[16];                ///< 运行时色表（init 时交给后端）
    
    /* 通道视频分辨率（画布钳位基准，默认 1920x1080） */
    int channelWidth_[2]  = {1920, 1920};
    int channelHeight_[2] = {1080, 1080};
    bool channelResSet_[2] = {false, false};
};

} // namespace osd

#endif // OSD_ENGINE_H
