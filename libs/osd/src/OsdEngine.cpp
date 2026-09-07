/**
 * OsdEngine.cpp - OSD 引擎实现
 * 
 * 功能：
 *   - 通过 pal::IOsdBackend 完成叠加子系统初始化/提交/销毁
 *   - 元素工厂：注入后端指针与通道分辨率后创建并首绘
 * 
 * 与原 OsdSdkManager 的行为差异仅两处：
 *   1. 静态全局状态收进实例；2. 厂商调用换成后端接口
 *      （"画字符串前重设字体文件"的约束下沉到后端 DrawWideStr 内部，
 *      工厂/重绘处的 refreshGlobalFont 调用随之移除，时序由后端保证）
 */

#include "OsdEngine.h"
#include "OsdElement.h"
#include "OsdTypes.h"  // HARDWARE_PALETTE
#include "OsdUtils.h"  // 颜色匹配工具函数
#include "pal/osd_backend.h"
#include <string.h>

namespace osd {

namespace {

/**
 * 内部日志辅助函数（回调未设置时静默丢弃）
 */
void engineLog(const ManagerConfig::LogCallback& cb, LogLevel level, const std::string& msg) {
    if (cb) {
        cb(level, msg);
    }
}

} // namespace

/**
 * 构造：收进后端与字体路径，字体立即交给后端记住
 * （后端在每次画字符串前自动重设，无需等到 init）
 */
OsdEngine::OsdEngine(std::unique_ptr<pal::IOsdBackend> backend, std::string fontFile)
    : backend_(std::move(backend))
    , fontFile_(std::move(fontFile))
{
    memset(colorTables_, 0, sizeof(colorTables_));
    if (backend_ && !fontFile_.empty()) {
        /* 16 = 字库点阵大小（16px 点阵字库），不是绘制字号 */
        backend_->SetFontFile(16, fontFile_);
    }
}

OsdEngine::~OsdEngine() = default;

/**
 * 设置日志回调（后续创建的元素都会带上这份回调）
 */
void OsdEngine::setLogCallback(ManagerConfig::LogCallback cb) {
    logCallback_ = std::move(cb);
}

/**
 * 更换字体文件：后端记住后，下一次画字符串即生效（后端每次画前重设）
 */
void OsdEngine::setFontFile(const std::string& path) {
    fontFile_ = path;
    if (backend_ && !fontFile_.empty()) {
        backend_->SetFontFile(16, fontFile_);
    }
}

/**
 * 初始化叠加子系统
 * 
 * 执行流程：
 *   1. 已初始化直接返回（叠加子系统只能初始化一次）
 *   2. 从 HARDWARE_PALETTE 复制色表，交给后端完成初始化
 */
int OsdEngine::init() {
    if (initialized_) {
        engineLog(logCallback_, LogLevel::Info, "OSD already initialized, skipping");
        return 0;
    }
    
    engineLog(logCallback_, LogLevel::Info, "Initializing OSD backend for the first time");
    
    /* 色表从硬件调色板常量复制（运行时可经 setColorTable 覆盖，下次 init 生效） */
    memcpy(colorTables_, HARDWARE_PALETTE, sizeof(colorTables_));
    
    /* 后端内部按平台配方组装初始化参数（anyka：RGB 色表/4 区域/4bit 色深） */
    uint32_t palette[16];
    for (int i = 0; i < 16; ++i) {
        palette[i] = colorTables_[i];
    }
    if (!backend_ || !backend_->Init(palette)) {
        engineLog(logCallback_, LogLevel::Error, "OSD backend init failed");
        return -1;
    }
    initialized_ = true;
    engineLog(logCallback_, LogLevel::Info, "OSD backend initialized successfully");
    return 0;
}

/**
 * 设置通道视频分辨率（元素建画布时的钳位基准）
 */
void OsdEngine::setChannelResolution(int channel, int width, int height) {
    if (channel < 0 || channel > 1) {
        engineLog(logCallback_, LogLevel::Warn, "setChannelResolution: invalid channel " + std::to_string(channel));
        return;
    }
    channelWidth_[channel] = width;
    channelHeight_[channel] = height;
    channelResSet_[channel] = true;
    engineLog(logCallback_, LogLevel::Info, "Channel " + std::to_string(channel) + 
           " resolution set to " + std::to_string(width) + "x" + std::to_string(height));
}

/**
 * 获取通道视频分辨率（未设置过返回 -1，调用方用默认值兜底）
 */
int OsdEngine::getChannelResolution(int channel, int& width, int& height) {
    if (channel < 0 || channel > 1 || !channelResSet_[channel]) {
        return -1;
    }
    width = channelWidth_[channel];
    height = channelHeight_[channel];
    return 0;
}

/**
 * 获取通道最大画布尺寸（问后端；查不到时上层用分辨率兜底）
 */
int OsdEngine::getMaxRectSize(int channel, int& maxWidth, int& maxHeight) {
    if (!backend_ || !backend_->GetMaxRect(channel, maxWidth, maxHeight)) {
        engineLog(logCallback_, LogLevel::Error, "backend GetMaxRect failed");
        return -1;
    }
    
    engineLog(logCallback_, LogLevel::Info, "Channel " + std::to_string(channel) + 
           " max rect size: " + std::to_string(maxWidth) + "x" + std::to_string(maxHeight));
    return 0;
}

/**
 * 转发后端的区域数量上限：不同平台叠加器容量不同（anyka 固定 4，
 * 软渲染宽松），服务层分槽前先问这里，不把平台限制写死在自己代码里。
 */
int OsdEngine::getMaxRegions() {
    if (!backend_) {
        return 0;
    }
    return backend_->GetMaxRegions();
}

/**
 * 查询是否已初始化
 */
bool OsdEngine::isInitialized() {
    return initialized_;
}

/**
 * 提交所有画布到 VI 显示
 */
int OsdEngine::commitToVi() {
    if (!backend_ || backend_->Commit() != 0) {
        engineLog(logCallback_, LogLevel::Error, "OSD commit to VI failed");
        return -1;
    }
    
    engineLog(logCallback_, LogLevel::Debug, "OSD canvases committed to VI");
    return 0;
}

/**
 * 销毁叠加子系统（相机重启前必须调：绑定的是旧 VI 会话）
 */
void OsdEngine::destroy() {
    engineLog(logCallback_, LogLevel::Info, "Destroying OSD backend resources");
    if (backend_) {
        backend_->Destroy();
    }
    initialized_ = false;
    engineLog(logCallback_, LogLevel::Info, "OSD backend destroyed");
}

/**
 * 重新绘制元素（统一入口）
 * 
 * 画之前先重设字体文件（对齐老项目 refreshGlobalFont 的做法）：
 * SDK 不缓存字体打开状态，动态更新销毁旧画布后新画布首次绘制时，
 * 后端 DrawWideStr 里虽然也会调，但在引擎层先刷一次更保险，
 * 避免新画布上首拍画出来只有后半截有字的情况。
 */
int OsdEngine::redrawElement(Element* elem) {
    if (!elem) {
        engineLog(logCallback_, LogLevel::Error, "redrawElement: elem is nullptr");
        return -1;
    }
    /* 重设字体：后端只记路径，真正生效在 DrawWideStr 里每次画前调，
     * 这里额外刷一次对齐老项目多层调用的习惯，防御画布重建后首拍字体异常 */
    if (backend_ && !fontFile_.empty()) {
        backend_->SetFontFile(16, fontFile_);
    }
    return elem->draw();
}

// ========== 元素创建方法实现 ==========

/**
 * 内部辅助：为元素注入后端指针与日志回调（与原实现创建即带回调一致）
 */
template<typename T>
T* prepareElement(T* elem, OsdEngine* engine, const ManagerConfig::LogCallback& logCb) {
    if (elem) {
        elem->setBackend(engine->backend());
        if (logCb) {
            elem->setLogCallback(logCb);
        }
    }
    return elem;
}

/**
 * 内部辅助：创建元素的标准流程（建画布 → 首绘，失败回滚）
 * 
 * 通道分辨率未设置时用 1920x1080 兜底（与原实现元素内兜底一致）
 * 
 * @return 创建成功返回元素，失败已回滚返回 nullptr
 */
template<typename T>
static T* createElementCommon(OsdEngine* engine, T* elem,
                              const ManagerConfig::LogCallback& logCb,
                              const char* name, int channel, int rect_id) {
    if (!elem) {
        engineLog(logCb, LogLevel::Error, std::string("Failed to create ") + name + " element");
        return nullptr;
    }
    
    int video_w = 1920, video_h = 1080;
    engine->getChannelResolution(channel, video_w, video_h);
    
    int ret = elem->create(channel, rect_id, video_w, video_h);
    if (ret != 0) {
        engineLog(logCb, LogLevel::Error, std::string(name) + "::create failed, ret=" + std::to_string(ret));
        delete elem;
        return nullptr;
    }
    
    /* create 内已首绘一次；与旧实现保持一致再画一拍（幂等，代价毫秒级） */
    ret = elem->draw();
    if (ret != 0) {
        engineLog(logCb, LogLevel::Error, std::string(name) + "::draw failed, ret=" + std::to_string(ret));
        elem->destroy();
        delete elem;
        return nullptr;
    }
    return elem;
}

/**
 * 创建时间元素
 */
Element* OsdEngine::createTimeElement(const TimeElementConfig& config, int channel, int rect_id) {
    engineLog(logCallback_, LogLevel::Info, "Creating time element, channel=" + std::to_string(channel) + 
           ", rect_id=" + std::to_string(rect_id));
    auto* elem = createElementCommon(this, prepareElement(new TimeElement(config), this, logCallback_),
                                     logCallback_, "TimeElement", channel, rect_id);
    if (elem) {
        engineLog(logCallback_, LogLevel::Info, "Time element created successfully");
    }
    return elem;
}

/**
 * 创建文本元素
 */
Element* OsdEngine::createTextElement(const TextElementConfig& config, int channel, int rect_id) {
    engineLog(logCallback_, LogLevel::Info, "Creating text element, channel=" + std::to_string(channel) + 
           ", rect_id=" + std::to_string(rect_id));
    auto* elem = createElementCommon(this, prepareElement(new TextElement(config), this, logCallback_),
                                     logCallback_, "TextElement", channel, rect_id);
    if (elem) {
        engineLog(logCallback_, LogLevel::Info, "Text element created successfully");
    }
    return elem;
}

/**
 * 创建矩形元素
 */
Element* OsdEngine::createRectElement(const RectConfig& config, int channel, int rect_id) {
    engineLog(logCallback_, LogLevel::Info, "Creating rect element, channel=" + std::to_string(channel) + 
           ", rect_id=" + std::to_string(rect_id));
    auto* elem = createElementCommon(this, prepareElement(new RectElement(config), this, logCallback_),
                                     logCallback_, "RectElement", channel, rect_id);
    if (elem) {
        engineLog(logCallback_, LogLevel::Info, "Rect element created successfully");
    }
    return elem;
}

/**
 * 创建圆形元素
 */
Element* OsdEngine::createCircleElement(const CircleConfig& config, int channel, int rect_id) {
    engineLog(logCallback_, LogLevel::Info, "Creating circle element, channel=" + std::to_string(channel) + 
           ", rect_id=" + std::to_string(rect_id));
    auto* elem = createElementCommon(this, prepareElement(new CircleElement(config), this, logCallback_),
                                     logCallback_, "CircleElement", channel, rect_id);
    if (elem) {
        engineLog(logCallback_, LogLevel::Info, "Circle element created successfully");
    }
    return elem;
}

/**
 * 创建椭圆元素
 */
Element* OsdEngine::createEllipseElement(const EllipseConfig& config, int channel, int rect_id) {
    engineLog(logCallback_, LogLevel::Info, "Creating ellipse element, channel=" + std::to_string(channel) + 
           ", rect_id=" + std::to_string(rect_id));
    auto* elem = createElementCommon(this, prepareElement(new EllipseElement(config), this, logCallback_),
                                     logCallback_, "EllipseElement", channel, rect_id);
    if (elem) {
        engineLog(logCallback_, LogLevel::Info, "Ellipse element created successfully");
    }
    return elem;
}

/**
 * 创建多边形元素（扫描线栅格化，占一个叠加区域）
 */
Element* OsdEngine::createPolygonElement(const PolygonConfig& config, int channel, int rect_id) {
    engineLog(logCallback_, LogLevel::Info, "Creating polygon element, channel=" + std::to_string(channel) + 
           ", rect_id=" + std::to_string(rect_id));
    auto* elem = createElementCommon(this, prepareElement(new PolygonElement(config), this, logCallback_),
                                     logCallback_, "PolygonElement", channel, rect_id);
    if (elem) {
        engineLog(logCallback_, LogLevel::Info, "Polygon element created successfully");
    }
    return elem;
}

/**
 * 创建位图元素（BMP 文件贴屏，占一个叠加区域）
 */
Element* OsdEngine::createBitmapElement(const BitmapConfig& config, int channel, int rect_id) {
    engineLog(logCallback_, LogLevel::Info, "Creating bitmap element, channel=" + std::to_string(channel) + 
           ", rect_id=" + std::to_string(rect_id) + ", path=" + config.image_path);
    auto* elem = createElementCommon(this, prepareElement(new BitmapElement(config), this, logCallback_),
                                     logCallback_, "BitmapElement", channel, rect_id);
    if (elem) {
        engineLog(logCallback_, LogLevel::Info, "Bitmap element created successfully");
    }
    return elem;
}

/**
 * 销毁元素（销毁画布 + 释放内存）
 */
void OsdEngine::destroyElement(Element* elem) {
    if (elem) {
        engineLog(logCallback_, LogLevel::Info, "Destroying element, type=" + std::to_string((int)elem->type));
        elem->destroy();
        delete elem;
        engineLog(logCallback_, LogLevel::Info, "Element destroyed");
    }
}

// ========== 颜色管理方法实现 ==========

/**
 * 将RGB颜色转换为硬件调色板索引（复用 OsdUtils 的匹配算法）
 */
int OsdEngine::matchColorIndex(int r, int g, int b, int alpha) {
    osd::Color color(alpha, r, g, b);
    return osd::matchColorIndex(color);
}

/**
 * 设置自定义颜色表（下次 init 生效，与旧实现语义一致）
 */
void OsdEngine::setColorTable(const unsigned int* colors) {
    if (colors) {
        memcpy(colorTables_, colors, sizeof(colorTables_));
        engineLog(logCallback_, LogLevel::Info, "Custom color table set");
    }
}

/**
 * 获取当前颜色表
 */
const unsigned int* OsdEngine::getColorTable() {
    return colorTables_;
}

} // namespace osd
