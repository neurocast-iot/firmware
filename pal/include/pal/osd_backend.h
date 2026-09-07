// pal/include/pal/osd_backend.h
// IOsdBackend：OSD 硬件叠加抽象接口（画布/位图/文字/提交）
// 接口形状按 nc::osd（libs/osd）元素类的实际用法定义：
//   建画布 → 画位图（图形元素）/ 画宽字符串（文字元素）→ 提交上屏
// 铁律：本文件零平台依赖，禁止 #include <ak_*.h>
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pal {

// OSD 画布描述：一块可叠加在视频帧上的索引色区域
// 对应厂商 SDK 的 region 概念：位置/尺寸单位像素；
// 每通道允许的 region 数量是平台能力（见 GetMaxRegions），不在此写死
struct OsdCanvas {
    int channel = 0;        // 视频通道号（0=主 1=子）
    int region_id = 0;      // 区域编号（每通道 0-3）
    int x = 0;              // 画布左上角在 YUV 帧上的 X（像素）
    int y = 0;              // 画布左上角在 YUV 帧上的 Y（像素）
    int width = 0;          // 画布宽（像素）
    int height = 0;         // 画布高（像素）
    int bg_color_index = 0; // 背景色在调色板中的索引（0=透明）
    int alpha = 0;          // 透明度 0~100（量纲取自 SDK：ak_osd_set_alpha 注释 0~100）：
                            // 0=不透明全显，值越大越透，100=完全看不见
};

// OSD 后端接口：由 pal/backends/* 提供具体实现（每个平台恰好一个）
//
// 字体约束（继承自硬件行为）：字符串绘制前必须重设字体文件，
// 叠加层内部不缓存字体的打开状态。该约束由后端在 DrawWideStr 内部
// 自行处理（用 SetFontFile 记住的文件），上层无需关心。
class IOsdBackend {
public:
    virtual ~IOsdBackend() = default;

    // 初始化 OSD 子系统，注入 16 色索引调色板（RGB 顺序与硬件一致）
    // 只能成功初始化一次；重复初始化前必须先 Destroy
    virtual bool Init(const uint32_t palette[16]) = 0;

    // 释放 OSD 子系统（绑定的是当前 VI 会话，相机重启后旧会话失效必须重走 Init）
    virtual void Destroy() = 0;

    // 创建/销毁一块画布（region）；同通道同 region_id 重复创建前须先销毁
    virtual bool CreateCanvas(const OsdCanvas& canvas) = 0;
    virtual void DestroyCanvas(int channel, int region_id) = 0;

    // 查询通道允许的画布最大尺寸（跟通道分辨率绑定，运行时现查）
    // @return false=查不到（调用方用通道分辨率兜底）
    virtual bool GetMaxRect(int channel, int& maxWidth, int& maxHeight) = 0;

    // 每通道允许同时存在的画布数量上限（平台能力，非全局常量）：
    // anyka 芯片固定 4 个（rect_id 0-3），软渲染/其他平台可以更大。
    // 上层按配置顺序分槽，超过此数量只能截断或放弃多余元素。
    virtual int GetMaxRegions() const = 0;

    // 将 4bpp 索引色位图写入画布（每字节 2 像素：偶数像素在高 4 位）
    // @param width/height 位图尺寸（像素），须与画布尺寸匹配
    // @return 0=成功，其余=失败
    virtual int DrawBitmap(int channel, int region_id, int width, int height,
                           const uint8_t* buffer, size_t len) = 0;

    // 记住字体文件（每次 DrawWideStr 前由后端自动重设，见类注释）
    // @param fontSize 字库点阵大小（如 16=16px 点阵），不是绘制字号
    virtual void SetFontFile(int fontSize, const std::string& path) = 0;

    // 设置通道绘制字号（实际绘制大小，字库整倍放大）
    // 调用约定：必须与 DrawWideStr 在同一线程内成对串行使用，
    // 即"设字号 → 紧跟画字"，中间不能插别的绘制（后端只记一份待生效字号，
    // 连续两次 SetFontSize 先设的会被盖掉）。当前由 OsdService 的锁保证。
    virtual void SetFontSize(int channel, int fontSize) = 0;

    // 设置文字前景/背景调色板索引（背景 0=透明）
    virtual void SetDrawColor(int fgIndex, int bgIndex) = 0;

    // 在画布上绘制 UTF-16 宽字符串（支持 \n 由上层拆行，此处按单行画）
    // @return 0=成功，<0=失败
    virtual int DrawWideStr(int channel, int region_id, int xoff, int yoff,
                            const uint16_t* str, int len) = 0;

    // 把全部画布提交到 VI 真正叠到画面上（改动不提交不上屏）
    // @return 0=成功，其余=失败
    virtual int Commit() = 0;
};

// 工厂：由当前编译的 backend 提供实现（每个 NC_PLATFORM 恰好链接一个 backend）
std::unique_ptr<IOsdBackend> CreateOsdBackend();

} // namespace pal
