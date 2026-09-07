// pal/backends/linux-generic/osd_backend_soft.cpp
// 软渲染 OSD 后端实现：画布存内存，位图真实写入，文字只记账。
// 行为语义对齐硬件后端：同通道同区域重复创建失败、位图尺寸须与画布一致、
// 未创建画布上绘制失败——单测才能验出上层逻辑错误而不是放水。
#include "osd_backend_soft.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace pal {

bool SoftOsdBackend::Init(const uint32_t paletteIn[16]) {
    for (int i = 0; i < 16; ++i) {
        palette[i] = paletteIn[i];
    }
    inited = true;
    return true;
}

/* 释放即清场：画布全清，与硬件 Destroy 后区域全没的语义一致 */
void SoftOsdBackend::Destroy() {
    canvases.clear();
    inited = false;
}

bool SoftOsdBackend::CreateCanvas(const OsdCanvas& canvas) {
    /* 同硬件语义：同通道同区域已有画布时创建失败（上层须先销毁） */
    if (findCanvas(canvas.channel, canvas.region_id) != nullptr) {
        return false;
    }
    CanvasRecord rec;
    rec.info = canvas;
    /* 4bpp：每字节 2 像素，总字节数向上取整 */
    rec.pixels.assign(static_cast<size_t>(canvas.width) * canvas.height / 2 +
                      (static_cast<size_t>(canvas.width) * canvas.height % 2), 0);
    canvases.push_back(std::move(rec));
    return true;
}

void SoftOsdBackend::DestroyCanvas(int channel, int region_id) {
    for (auto it = canvases.begin(); it != canvases.end(); ++it) {
        if (it->info.channel == channel && it->info.region_id == region_id) {
            canvases.erase(it);
            return;
        }
    }
}

/* 返回配置的通道分辨率（模拟硬件"上限跟分辨率绑定"的行为） */
bool SoftOsdBackend::GetMaxRect(int channel, int& maxWidth, int& maxHeight) {
    if (channel < 0 || channel > 1) {
        return false;
    }
    maxWidth = channelWidth_[channel];
    maxHeight = channelHeight_[channel];
    return true;
}

/* 软渲染画布全在内存，没有硬件叠加器的区域数量限制，
 * 返回宽松上限（测试可调小验证上层截断逻辑） */
int SoftOsdBackend::GetMaxRegions() const {
    return maxRegions_;
}

void SoftOsdBackend::setMaxRegions(int n) {
    maxRegions_ = (n < 1) ? 1 : n;
}

/**
 * 位图真实写入画布内存：尺寸与画布不符按硬件语义失败，
 * 写入后单测可逐像素断言几何结果。
 */
int SoftOsdBackend::DrawBitmap(int channel, int region_id, int width, int height,
                               const uint8_t* buffer, size_t len) {
    CanvasRecord* rec = const_cast<CanvasRecord*>(findCanvas(channel, region_id));
    if (rec == nullptr || width != rec->info.width || height != rec->info.height ||
        buffer == nullptr) {
        return -1;
    }
    size_t copyLen = std::min(len, rec->pixels.size());
    memcpy(rec->pixels.data(), buffer, copyLen);
    ++drawBitmapCalls;
    return 0;
}

void SoftOsdBackend::SetFontFile(int fontSize, const std::string& path) {
    fontFileSize_ = fontSize;
    fontFile_ = path;
}

void SoftOsdBackend::SetFontSize(int channel, int fontSize) {
    if (channel >= 0 && channel <= 1) {
        fontSize_[channel] = fontSize;
    }
}

void SoftOsdBackend::SetDrawColor(int fgIndex, int bgIndex) {
    fgIndex_ = fgIndex;
    bgIndex_ = bgIndex;
}

/* 无字库不渲染字形，只记录字符串内容（断言"画了什么字"够用了） */
int SoftOsdBackend::DrawWideStr(int channel, int region_id, int xoff, int yoff,
                                const uint16_t* str, int len) {
    (void)xoff;
    (void)yoff;
    if (findCanvas(channel, region_id) == nullptr || str == nullptr || len < 0) {
        return -1;
    }
    lastDrawStr.assign(str, str + len);
    ++drawStrCalls;
    return 0;
}

int SoftOsdBackend::Commit() {
    ++commitCalls;
    return 0;
}

void SoftOsdBackend::setChannelResolution(int channel, int w, int h) {
    if (channel >= 0 && channel <= 1) {
        channelWidth_[channel] = w;
        channelHeight_[channel] = h;
    }
}

const SoftOsdBackend::CanvasRecord*
SoftOsdBackend::findCanvas(int channel, int region_id) const {
    for (const auto& rec : canvases) {
        if (rec.info.channel == channel && rec.info.region_id == region_id) {
            return &rec;
        }
    }
    return nullptr;
}

std::unique_ptr<IOsdBackend> CreateOsdBackend() {
    return std::make_unique<SoftOsdBackend>();
}

} // namespace pal
