// pal/backends/linux-generic/osd_backend_soft.h
// 软渲染 OSD 后端（linux-generic / x86 宿主）：无硬件叠加器，画布放内存。
// 用途：让平台无关的 nc::osd 在 x86 上可编译、可跑生命周期单测；
//       仅编进 linux-generic 的 nc_pal_backend，ARM 设备二进制不含本文件。
// 与真后端的差异：DrawBitmap 真实写入内存像素缓冲（几何结果可断言），
// DrawWideStr 只记录字符串内容（无字库），其余方法记调用计数。
#pragma once

#include "pal/osd_backend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pal {

class SoftOsdBackend : public IOsdBackend {
public:
    // 一块内存画布：描述信息 + DrawBitmap 写入的 4bpp 像素数据
    struct CanvasRecord {
        OsdCanvas info;
        std::vector<uint8_t> pixels;   ///< 尺寸 = width*height/2（向上取整）
    };

    bool Init(const uint32_t palette[16]) override;
    void Destroy() override;
    bool CreateCanvas(const OsdCanvas& canvas) override;
    void DestroyCanvas(int channel, int region_id) override;
    bool GetMaxRect(int channel, int& maxWidth, int& maxHeight) override;
    int GetMaxRegions() const override;
    int DrawBitmap(int channel, int region_id, int width, int height,
                   const uint8_t* buffer, size_t len) override;
    void SetFontFile(int fontSize, const std::string& path) override;
    void SetFontSize(int channel, int fontSize) override;
    void SetDrawColor(int fgIndex, int bgIndex) override;
    int DrawWideStr(int channel, int region_id, int xoff, int yoff,
                    const uint16_t* str, int len) override;
    int Commit() override;

    // ---- 测试侧配置与内省（生产路径不使用）----

    /* GetMaxRect 的返回来源（默认 1920x1080，模拟通道分辨率） */
    void setChannelResolution(int channel, int w, int h);

    /* 修改区域数量上限（默认 16：软渲染无硬件限制，测试可改小验证截断逻辑） */
    void setMaxRegions(int n);

    /* 按通道+区域号找画布记录，找不到返回 nullptr */
    const CanvasRecord* findCanvas(int channel, int region_id) const;

    int canvasCount() const { return static_cast<int>(canvases.size()); }

    std::vector<CanvasRecord> canvases;   ///< 现存画布列表
    uint32_t palette[16] = {0};           ///< Init 注入的调色板
    bool inited = false;                  ///< 是否已初始化
    int drawBitmapCalls = 0;              ///< DrawBitmap 成功次数
    int drawStrCalls = 0;                 ///< DrawWideStr 成功次数
    int commitCalls = 0;                  ///< Commit 次数
    std::vector<uint16_t> lastDrawStr;    ///< 最近一次绘制的宽字符串内容

private:
    int channelWidth_[2]  = {1920, 1920};
    int channelHeight_[2] = {1080, 1080};
    int maxRegions_ = 16;                 ///< 软渲染无硬件叠加器，给个宽松默认值
    int fontSize_[2] = {16, 16};          ///< 各通道最近设置的绘制字号
    int fgIndex_ = 0;                     ///< 最近设置的前景索引
    int bgIndex_ = 0;                     ///< 最近设置的背景索引
    int fontFileSize_ = 16;               ///< SetFontFile 记住的字库点阵大小
    std::string fontFile_;                ///< SetFontFile 记住的字体路径
};

} // namespace pal
