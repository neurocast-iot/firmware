/**
 * osd_engine_smoke_test.cpp - OsdEngine 生命周期冒烟测试（软渲染后端）
 *
 * 覆盖路径：引擎构造 → init → 通道分辨率 → 建时间/矩形/多边形/位图元素（建画布 + 首绘）
 *   → setTimeText + update 重绘 → commit → 销毁元素 → 销毁叠加子系统。
 * 断言走软后端内省接口（canvases/drawBitmapCalls/drawStrCalls/commitCalls），
 * 验证 libs/osd 全部绘制动作确实经 pal 接口下沉到了后端；
 * 多边形/位图额外做逐像素几何断言（填充索引、就近色量化结果）。
 *
 * 不依赖 gtest（嵌入式仓库依赖最小化）：main + CHECK 宏，任一断言失败退出码非 0。
 */
#include "osd/OsdEngine.h"
#include "osd/OsdElement.h"
#include "osd/OsdTypes.h"
#include "osd_backend_soft.h"   /* 内省接口仅测试用，include 路径由 tests/CMakeLists 提供 */

#include <cstdio>
#include <memory>
#include <vector>

static int g_failed = 0;

/* 断言失败打印行号与表达式后继续跑完剩余检查，一次运行暴露全部问题 */
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failed;                                                  \
        }                                                                \
    } while (0)

/* 4bpp 点阵取像素索引（偶数像素在高 4 位，奇数在低 4 位） */
static int getPixel4(const std::vector<uint8_t>& pixels, int stride, int x, int y) {
    const int idx = y * stride + x;
    const uint8_t b = pixels[static_cast<size_t>(idx) / 2];
    return (idx % 2 == 0) ? (b >> 4) : (b & 0x0F);
}

/* 写一个 24 位无压缩 BMP 到 /tmp（不依赖仓库里的 demo 文件，测试自含）：
 * 全部像素纯黄（BGR=0,255,255），尺寸偶数保证 4bpp 对整齐 */
static bool writeTestBmp(const char* path, int w, int h) {
    std::vector<uint8_t> row(static_cast<size_t>(w) * 3, 0);
    for (int x = 0; x < w; ++x) {
        row[x * 3 + 0] = 0;      /* B */
        row[x * 3 + 1] = 255;    /* G */
        row[x * 3 + 2] = 255;    /* R */
    }
    row.resize((static_cast<size_t>(w) * 3 + 3) & ~size_t(3));   /* 行补齐 4 字节 */

    std::vector<uint8_t> f(54);
    const uint32_t pixBytes = static_cast<uint32_t>(row.size()) * h;
    auto wr32 = [&f](int off, uint32_t v) {
        f[off] = v & 0xFF; f[off + 1] = (v >> 8) & 0xFF;
        f[off + 2] = (v >> 16) & 0xFF; f[off + 3] = (v >> 24) & 0xFF;
    };
    f[0] = 'B'; f[1] = 'M';
    wr32(2, 54 + pixBytes);   /* 文件总长 */
    wr32(10, 54);             /* 像素数据偏移 */
    wr32(14, 40);             /* 信息头长 */
    wr32(18, w); wr32(22, h);
    f[28] = 24;               /* 位深 */
    wr32(34, pixBytes);
    for (int i = 0; i < h; ++i) {   /* BMP 从最后一行往上存 */
        f.insert(f.end(), row.begin(), row.end());
    }
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;
    const bool ok = std::fwrite(f.data(), 1, f.size(), fp) == f.size();
    std::fclose(fp);
    return ok;
}

int main() {
    /* 软后端裸指针仅用于内省断言；所有权交给引擎的 unique_ptr */
    auto* softRaw = new pal::SoftOsdBackend;
    std::unique_ptr<pal::IOsdBackend> backend(softRaw);

    /* 字体路径随意：软后端只记住不加载字库（真后端每次画字符串前重设它） */
    osd::OsdEngine engine(std::move(backend), "/tmp/fake_font.bin");

    /* 钳位基准两边同步：引擎侧供元素 create 用，后端侧是 GetMaxRect 的返回来源 */
    engine.setChannelResolution(0, 1280, 720);
    softRaw->setChannelResolution(0, 1280, 720);

    CHECK(engine.init() == 0);
    CHECK(softRaw->inited);

    /* ---- 时间元素：建画布成功即上屏。
     * 库契约：时间串由上层喂（不喂不画，避免元素自己读时钟），
     * 所以创建后首绘被跳过，画字符串的断言放到下面喂时间之后 ---- */
    osd::TimeElementConfig tc;
    tc.at(10, 10).withSize(320, 40).withFontSize(32).withColor(osd::Color::White());
    osd::Element* timeElem = engine.createTimeElement(tc, 0, 0);
    CHECK(timeElem != nullptr);
    CHECK(softRaw->canvasCount() == 1);
    CHECK(softRaw->drawStrCalls == 0);   /* 未喂时间：首绘被跳过，符合“不喂不画”契约 */

    /* ---- 矩形元素：建画布 + DrawBitmap 真实写入内存像素 ---- */
    osd::RectConfig rc;
    rc.at(100, 100).withSize(200, 150).borderWidth(0).withFill(osd::Color::Red());
    osd::Element* rectElem = engine.createRectElement(rc, 0, 2);
    CHECK(rectElem != nullptr);
    CHECK(softRaw->canvasCount() == 2);
    CHECK(softRaw->drawBitmapCalls >= 1);
    const auto* rectCanvas = softRaw->findCanvas(0, 2);
    CHECK(rectCanvas != nullptr);
    CHECK(rectCanvas != nullptr && !rectCanvas->pixels.empty());
    /* 透明度透传断言：矩形不显式设就用默认 20（遮盖块半透，量纲 0~100） */
    CHECK(rectCanvas != nullptr && rectCanvas->info.alpha == 20);

    /* ---- 多边形元素：扫描线栅格化后走 DrawBitmap。
     * 顶点全取偶数坐标：包围盒宽高为奇数时元素内部按偶数 stride 填，
     * 软后端只拷画布名义像素数，奇数宽会截尾导致断言不可靠，避开它 ---- */
    osd::PolygonConfig pc;
    pc.withPoints({{100, 100}, {198, 100}, {148, 198}})
      .withFill(osd::Color::Green());
    osd::Element* polyElem = engine.createPolygonElement(pc, 0, 3);
    CHECK(polyElem != nullptr);
    CHECK(softRaw->canvasCount() == 3);
    const auto* polyCanvas = softRaw->findCanvas(0, 3);
    CHECK(polyCanvas != nullptr);
    CHECK(polyCanvas != nullptr && !polyCanvas->pixels.empty());
    /* 多边形默认和矩形遮盖同语义：透明度 20 */
    CHECK(polyCanvas != nullptr && polyCanvas->info.alpha == 20);
    if (polyCanvas && !polyCanvas->pixels.empty()) {
        const int greenIdx = 4;   /* 绿色在硬件调色板的固定索引 */
        const int stride = 100;   /* 包围盒宽 99 → 偶数对齐 100 */
        /* 重心附近必在形内（148 是画布局部坐标：148-100=48，重心行 133-100=33） */
        CHECK(getPixel4(polyCanvas->pixels, stride, 48, 60) == greenIdx);
        /* 顶部水平边首行也被填上：扫描线半开区间在 p.y==scan_y 侧含边界，
         * 水平边不会重复计数，首行填充属预期行为 */
        CHECK(getPixel4(polyCanvas->pixels, stride, 0, 0) == greenIdx);
        CHECK(getPixel4(polyCanvas->pixels, stride, 98, 0) == greenIdx);
        /* 形外区域保持背景透明索引 0（顶边两侧斜边外的肩角） */
        CHECK(getPixel4(polyCanvas->pixels, stride, 0, 50) == 0);
        CHECK(getPixel4(polyCanvas->pixels, stride, 98, 50) == 0);
    }

    /* ---- 位图元素：手写 BMP 解析 + 就近色量化。测试自写临时 BMP（纯黄 16x16） */
    const char* bmpPath = "/tmp/osd_smoke_test.bmp";
    CHECK(writeTestBmp(bmpPath, 16, 16));
    osd::BitmapConfig bc;
    bc.fromFile(bmpPath).at(300, 50).withSize(32, 32)   /* 放大采样也要盖到 */
      .withOpacity(30);                                  /* 显式透明度透传验证 */
    osd::Element* bitmapElem = engine.createBitmapElement(bc, 0, 4);
    CHECK(bitmapElem != nullptr);
    CHECK(softRaw->canvasCount() == 4);
    const auto* bmpCanvas = softRaw->findCanvas(0, 4);
    CHECK(bmpCanvas != nullptr);
    CHECK(bmpCanvas != nullptr && bmpCanvas->info.alpha == 30);
    if (bmpCanvas && !bmpCanvas->pixels.empty()) {
        const int yellowIdx = 6;   /* 纯黄在硬件调色板的固定索引 */
        CHECK(getPixel4(bmpCanvas->pixels, 32, 0, 0) == yellowIdx);
        CHECK(getPixel4(bmpCanvas->pixels, 32, 31, 31) == yellowIdx);
    }
    /* 文件不存在只让这一个元素失败（不影响其他），断言返回空 */
    osd::BitmapConfig bcBad;
    bcBad.fromFile("/tmp/osd_smoke_no_such.bmp").at(0, 0).withSize(16, 16);
    CHECK(engine.createBitmapElement(bcBad, 0, 5) == nullptr);

    /* ---- 刷新通用化契约：喂时间后 update() 触发一次重绘。
     * 创建时未喂时间，首绘被库跳过（契约：不喂不画），
     * 这里喂后第一拍才产生 drawStr 调用 ---- */
    const int strBefore = softRaw->drawStrCalls;
    static_cast<osd::TimeElement*>(timeElem)->setTimeText("2026-01-01 00:00:00");
    CHECK(timeElem->update() == 0);
    CHECK(softRaw->drawStrCalls == strBefore + 1);
    /* 画出来的宽字符串内容与喂进去的一致（ASCII 逐字节对齐低 8 位） */
    CHECK(!softRaw->lastDrawStr.empty());
    CHECK(softRaw->lastDrawStr.front() == '2' && softRaw->lastDrawStr.back() == '0');

    /* 基类 update() 默认空操作：矩形不重绘、不碰后端（将来加周期性元素的依据） */
    const int bmpBefore = softRaw->drawBitmapCalls;
    CHECK(rectElem->update() == 0);
    CHECK(softRaw->drawBitmapCalls == bmpBefore);

    /* ---- 提交与销毁序列（对齐 OsdService 的 stop 路径） ---- */
    CHECK(engine.commitToVi() == 0);
    CHECK(softRaw->commitCalls == 1);

    engine.destroyElement(timeElem);
    engine.destroyElement(rectElem);
    engine.destroyElement(polyElem);
    engine.destroyElement(bitmapElem);
    CHECK(softRaw->canvasCount() == 0);

    engine.destroy();
    CHECK(!softRaw->inited);

    if (g_failed == 0) {
        std::printf("osd_engine_smoke_test: all checks passed\n");
        return 0;
    }
    std::printf("osd_engine_smoke_test: %d check(s) FAILED\n", g_failed);
    return 1;
}
