/**
 * @file OsdService.cpp
 * @brief OSD 水印服务实现
 *
 * 生命周期与拍照/录像服务对齐：
 *   相机就绪 → start()（按 osd.enabled 决定是否上屏）
 *   配置热更 → apply()（enabled 开关翻转 / 参数变更动态重建）
 *   相机重启 → pauseForRestart() / resumeAfterRestart()
 *   进程退出 → stop()
 *
 * 跨分辨率换算（配置存千分比意图，这里换成像素）：
 *   字号 = 画面高 × 档位比例 → 就近取 16 的整倍数（点阵字库只支持整倍放大）
 *   位置/画布 = 千分比 × 通道实际宽/高，再双钳位：
 *   ① 不超通道分辨率（库内还会再钳一道）② 不超 SDK 每通道画布上限 max_rect
 *
 * 刷新线程只在时间元素存在时运行：每秒按配置格式拼好时间字符串，
 * 先调 setTimeText() 喂给时间元素再重绘（库本身不读时钟也不重排格式，
 * 不喂时间水印就是空白）。文本/图形元素不消耗线程。
 *
 * 已知硬件约束（2026-08 设备实测）：子通道（通道 1）的 OSD 画布宽度上限
 * 固定 320 像素——主/子都是 1280x720 时 max_rect 返回 320x720，子改 640x480
 * 后返回 320x480，高度跟通道走、宽度恒 320，与已分配画布多少无关。
 * SDK 没有任何参数能调大（osd_attr 无内存池字段，ak_mem 也不管 OSD），
 * 子通道上超 320 宽的水印必然被钳小，主/子观感无法完全一致时以钳位结果为准。
 */
#include "services/OsdService.h"

#include "nc/common/log_utils.h"

#include "osd/OsdElement.h"
#include "osd/OsdEngine.h"
#include "osd/OsdTypes.h"
#include "osd/OsdUtils.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace mediad {

namespace {

/* 字符串“字宽单位数”：直接调 osd 库的共用实现（口径只此一份，
 * 与画字时的画布宽度算法保持一致，避免两处各改各的产生漂移） */
int charUnits(const std::string& s) {
    return osd::utf8WidthUnits(s);
}

/* 生成当前时间字符串（库不重排格式，按配置格式在这里拼好再喂）。
 * 两路通道喂同一份字符串，保证显示同一时刻。 */
std::string currentTimeString(const std::string& format, bool showWeek) {
    /* 周日/周一/.../周六（GB2312），两个格式分支共用 */
    static const char* weeks[7] = {
        " \xD6\xDC\xC8\xD5", " \xD6\xDC\xD2\xBB", " \xD6\xDC\xB6\xFE",
        " \xD6\xDC\xC8\xFD", " \xD6\xDC\xCB\xC4", " \xD6\xDC\xCE\xE5",
        " \xD6\xDC\xC1\xF9"
    };
    time_t now = std::time(nullptr);
    struct tm tmInfo;
    localtime_r(&now, &tmInfo);
    char buf[32];
    const char* fmt = "%Y-%m-%d %H:%M:%S";
    if (format == "MM-DD-YYYY") {
        fmt = "%m-%d-%Y %H:%M:%S";
    } else if (format == "Chinese") {
        /* 中文格式里“年月日”是 GB2312 双字节字符，strftime 模板塞不进去，
         * 拆成三段数字分别格式化再拼起来（字节序列与库内中文格式一致） */
        char d[16], t[16];
        if (strftime(d, sizeof(d), "%Y", &tmInfo) == 0) return "";
        std::string s = std::string(d) + "\xC4\xEA";     /* 年 */
        if (strftime(d, sizeof(d), "%m", &tmInfo) == 0) return "";
        s += d;
        s += "\xD4\xC2";                                  /* 月 */
        if (strftime(d, sizeof(d), "%d", &tmInfo) == 0) return "";
        s += d;
        s += "\xC8\xD5 ";                                 /* 日 */
        if (strftime(t, sizeof(t), "%H:%M:%S", &tmInfo) == 0) return "";
        s += t;
        if (showWeek) {
            s += weeks[tmInfo.tm_wday];
        }
        return s;
    }
    if (strftime(buf, sizeof(buf), fmt, &tmInfo) == 0) {
        return "";   /* strftime 失败极罕见，空串时元素画空白，下一拍自愈 */
    }
    std::string s = buf;
    if (showWeek) {
        s += weeks[tmInfo.tm_wday];
    }
    return s;
}

} // namespace

/**
 * 构造：收下引擎（内含平台后端），生命周期归服务管（析构时随 stop() 释放叠加子系统）
 */
OsdService::OsdService(std::unique_ptr<osd::OsdEngine> engine)
    : m_engine(std::move(engine)) {
}

OsdService::~OsdService() {
    stop();
}

/**
 * 每通道画布双钳位：画布宽高不超引擎查到的该通道上限（后端转问硬件，
 * 跟分辨率绑定、运行时现查），位置超界就往回缩。
 * 旧项目从不查这个上限，小分辨率通道上画布超限直接创建失败。
 * 注意：子通道返回的宽度恒为 320（硬件固定上限，见文件头说明），
 * 不是共享内存被主通道挤占，钳位照做即可。
 * @return false=查不到上限，用分辨率兑底继续（不阻断上屏）
 * @note 要求已持 m_mutex（clampCanvasLocked 名字带 Locked 就是这个约定）
 */
bool OsdService::clampCanvasLocked(int channel, int videoW, int videoH,
                                   int& x, int& y, int& w, int& h) {
    int maxW = videoW, maxH = videoH;
    bool ok = (m_engine->getMaxRectSize(channel, maxW, maxH) == 0);
    if (!ok) {
        /* 查不到就用分辨率当上限，至少不越界 */
        NC_LOGW("OsdService: getMaxRectSize failed, channel={}, fallback to resolution", channel);
    }
    /* 复用库内统一钳位逻辑，口径和 clampToBounds 测试保持一致 */
    auto bounds = osd::clampToBounds(x, y, w, h, maxW, maxH, videoW, videoH);
    x = bounds.x; y = bounds.y; w = bounds.width; h = bounds.height;
    return ok;
}

/**
 * 按配置拉起水印：停旧刷新线程 → 存配置 → 补引擎初始化 → 重建元素 → 按需起线程。
 *
 * 为什么不拆成更细的接口：相机就绪、配置热更、相机重启后恢复三个入口都走这里，
 * 内部按当前状态（SDK 是否初始化、线程是否在跑）自己补齐，调用方不用管顺序。
 *
 * @return false 仅表示 SDK 初始化或元素重建失败，不致命；
 *         下次配置下发或相机重启会再走一遍自动重试。
 * @note 调用前不能持 m_mutex（内部 stopRefreshThread 要 join 刷新线程，
 *       而线程内部要取这把锁，持锁进来会死锁）
 */
bool OsdService::start(const OsdCfg& cfg, const ChannelCfg& main, const ChannelCfg& sub) {
    /* 先把旧刷新线程收掉：重建期间不能有线程在画布上重绘，
     * 且线程内部要取 m_mutex，必须先放开锁再 join */
    stopRefreshThread();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_cfg     = cfg;
    m_mainRes = main;
    m_subRes  = sub;

    if (!m_engine) {
        /* 构造时未注入引擎属编程错误，服务永不退出，降级为水印不可用 */
        NC_LOGE("OsdService: engine not injected, osd unavailable");
        return false;
    }

    /* 字体路径每次都重设给引擎：动态更新会销毁旧画布再建新画布，
     * SDK 内部字体打开状态可能随画布销毁失效，不重设的话新建的时间元素
     * 画出来可能只有后半截有字。去掉"路径变了才设"的判断，每次 start 都刷 */
    if (!cfg.fontFile.empty()) {
        m_engine->setFontFile(cfg.fontFile);
        m_curFontFile = cfg.fontFile;
    }

    /* 开关关着：不初始化叠加子系统、不起任何线程，视为按配置生效 */
    if (!cfg.enabled) {
        destroyElementsLocked();
        m_active = false;
        NC_LOGI("OsdService: disabled by config");
        return true;
    }

    if (!m_sdkInited) {
        /* 引擎与元素的日志接入 nc 日志系统（引擎会透传给后续创建的元素） */
        m_engine->setLogCallback(
            [](osd::LogLevel level, const std::string& msg) {
                switch (level) {
                    case osd::LogLevel::Error: NC_LOGE("[osd] {}", msg.c_str()); break;
                    case osd::LogLevel::Warn:  NC_LOGW("[osd] {}", msg.c_str()); break;
                    case osd::LogLevel::Debug: NC_LOGD("[osd] {}", msg.c_str()); break;
                    default:                   NC_LOGI("[osd] {}", msg.c_str()); break;
                }
            });
        int ret = m_engine->init();
        if (ret != 0) {
            /* 初始化失败不致命（录像/拍照不受影响），下次配置下发
             * 或相机重启后重新拉起时自动重试 */
            NC_LOGE("OsdService: OsdEngine::init failed, ret={}", ret);
            return false;
        }
        m_sdkInited = true;
    }

    if (!rebuildElementsLocked()) {
        return false;
    }

    /* 秒级刷新线程只在时间元素存在时启动（文本/图形元素零线程开销） */
    if (!m_timeElems.empty() && !m_thread.joinable()) {
        m_stopFlag = false;
        m_thread = std::thread(&OsdService::refreshLoop, this);
    }
    return true;
}

/**
 * 配置热更入口：配置下发后拿新配置和主/子分辨率进来，服务自己判断怎么走。
 *
 * 为什么区分两条路：关开关是唯一要"只清屏不重建"的场景（元素销毁完就完事，
 * 不起线程）；其余情况（开开关、改参数、改分辨率）统一走 start 全量重建，
 * 避免漏改某个元素造成新旧元素混在屏上。
 *
 * @return false 同 start：走重建路径失败，下次下发自动重试。
 * @note 调用前不能持 m_mutex，理由同 start()
 */
bool OsdService::apply(const OsdCfg& cfg, const ChannelCfg& main, const ChannelCfg& sub) {
    /* true→false：停线程 + 清空在屏元素 + 清屏，SDK 保持初始化（再开秒回） */
    bool disableCase = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        disableCase = m_sdkInited && m_cfg.enabled && !cfg.enabled;
        m_mainRes   = main;
        m_subRes    = sub;
    }
    if (disableCase) {
        stopRefreshThread();
        std::lock_guard<std::mutex> lock(m_mutex);
        destroyElementsLocked();
        m_cfg = cfg;
        m_engine->commitToVi();   /* 把清屏结果刷到 VI */
        m_active = false;
        NC_LOGI("OsdService: watermark disabled");
        return true;
    }
    /* 其余情况（false→true、参数变化、分辨率变化）统一走 start 路径：
     * 内部处理“收旧线程 → 引擎补初始化 → 元素重建 → 按需起线程” */
    return start(cfg, main, sub);
}

/**
 * 相机重启前的清场：停刷新线程、销毁在屏元素、释放叠加子系统。
 *
 * 为什么后端也要释放：叠加子系统初始化时绑定的是当前 VI 会话，相机重启后
 * 旧会话失效，不释放再画就写到失效的画布上。释放后 start/resume 会重新初始化。
 *
 * @note CameraService 重启流程里调用，必须在真正重启相机之前；
 *       调用后再碰元素就是空指针（已清空）。幂等：没初始化过直接返回。
 */
void OsdService::pauseForRestart() {
    /* 先停线程：旧 VI 上下文即将失效，不能让线程再碰 SDK */
    stopRefreshThread();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sdkInited) {
        return;   /* 未初始化过（禁用态），无场可清 */
    }
    destroyElementsLocked();
    /* 释放叠加子系统：后端 init 绑定 VI 会话，相机重启后旧上下文失效，
     * 置位后下次 resumeAfterRestart/start 会重走引擎 init */
    m_engine->destroy();
    m_sdkInited = false;
    m_active = false;
    NC_LOGI("OsdService: paused for camera restart (backend released)");
}

/**
 * 相机重启完成后恢复水印：pauseForRestart 已把 SDK 置为未初始化，
 * 直接走 start 会重走 init + 重建，不用单独写恢复逻辑。
 *
 * @note 必须在相机新通道就绪后调用，否则拿不到正确的通道分辨率。
 */
bool OsdService::resumeAfterRestart(const OsdCfg& cfg, const ChannelCfg& main, const ChannelCfg& sub) {
    /* pauseForRestart 已把 SDK 置为未初始化，这里走 start 会重走 init + 重建 */
    return start(cfg, main, sub);
}

/**
 * 全量重建在屏元素：先销毁旧的，按配置数组顺序分槽逐个建出来。
 *
 * 分槽规则：rect_id 从 0 起按配置数组顺序分配（关掉的、没内容的不占槽），
 * 每通道能用几个槽问平台后端（anyka 芯片固定 4，其他平台可能更多），
 * 超量的跳过并告警一次；将来接容量更大的平台，这段代码不用改。
 *
 * 为什么全量重建不做增量：元素个数和位置跟配置、分辨率都相关，逐个对比新旧
 * 差异容易漏（尤其分辨率变了所有元素位置都要动），全量重建最可靠，
 * 代价是重建瞬间水印短暂消失（毫秒级）可接受。
 *
 * @note 只能在持 m_mutex 时调用（名字带 Locked 就是这个约定）；
 *       刷新线程必须已停，否则会和重建抢画布。
 * @return 目前恒 true：单个元素失败只跳过该元素不阻断整体，
 *         是否真的有元素上屏看 m_active。
 */
bool OsdService::rebuildElementsLocked() {
    destroyElementsLocked();

    /* 对齐老项目：销毁旧元素后把叠加子系统也整个销毁再重建。
     * SDK 内部的字体/画布缓存等状态随画布销毁可能残留脏数据，
     * 不重建的话新建的时间元素首拍画出来只有后半截有字。
     * 老项目 cleanup() 里就是这么做的：destroy + init 全套重来 */
    m_engine->destroy();
    if (m_engine->init() != 0) {
        NC_LOGE("OsdService: SDK re-init failed after element rebuild");
        return false;
    }

    /* 分辨率钳位基准：主/子通道分别设置（画布越界保护，支持动态更新） */
    m_engine->setChannelResolution(0, m_mainRes.width, m_mainRes.height);
    m_engine->setChannelResolution(1, m_subRes.width, m_subRes.height);

    /* 槽位上限问平台后端：anyka 芯片固定 4（rect_id 0-3），
     * 软渲染和将来的平台可能更多；不在此写死平台常量 */
    const int maxRegions = m_engine->getMaxRegions();
    int usedSlots = 0;
    int createdCount = 0;
    bool overflowWarned = false;

    for (const auto& ec : m_cfg.elements) {
        /* 关掉的、没内容可画的元素不占硬件槽位，不挤掉后面的 */
        if (!ec.enabled || (ec.type == "label" && ec.text.empty())) {
            continue;
        }
        if (maxRegions > 0 && usedSlots >= maxRegions) {
            /* 超上限：后面的全部跳过，只告警一次（硬件就画不下这么多） */
            if (!overflowWarned) {
                NC_LOGW("OsdService: elements exceed platform limit {}, rest dropped",
                        maxRegions);
                overflowWarned = true;
            }
            continue;
        }
        const int rectId = usedSlots;

        for (int ch = 0; ch < 2; ++ch) {
            osd::Element* elem = nullptr;
            if (ec.type == "time") {
                elem = createTimeElemLocked(ec, ch, rectId);
            } else if (ec.type == "label") {
                elem = createLabelElemLocked(ec, ch, rectId);
            } else if (ec.type == "polygon") {
                elem = createPolygonElemLocked(ec, ch, rectId);
            } else if (ec.type == "bitmap") {
                elem = createBitmapElemLocked(ec, ch, rectId);
            } else {
                elem = createShapeElemLocked(ec, ch, rectId);
            }
            if (!elem) {
                /* 单个元素失败只跳过，其余照常上屏 */
                NC_LOGE("OsdService: create element failed, type={}, id={}, channel={}",
                        ec.type.c_str(), ec.id.c_str(), ch);
                continue;
            }
            ++createdCount;
            m_allElems.push_back(elem);
            /* 时间元素连格式一起登记：刷新线程按各自格式拼时间串喂入，
             * 多个时间元素可以各带各的格式 */
            if (ec.type == "time") {
                m_timeElems.push_back(TimeElemSlot{elem, ec.format, ec.showWeek});
            }
        }
        ++usedSlots;
    }

    /* 时间元素先喂一版当前时间并立即重绘，避免刷新线程第一拍之前显示空白；
     * 每个元素可能带不同格式，逐个拼 */
    for (auto& slot : m_timeElems) {
        const std::string now = currentTimeString(slot.format, slot.showWeek);
        static_cast<osd::TimeElement*>(slot.elem)->setTimeText(now);
        m_engine->redrawElement(slot.elem);
    }

    if (m_engine->commitToVi() != 0) {
        /* 提交失败记警告不致命：元素已建好，下一拍重绘或下次重建可恢复 */
        NC_LOGW("OsdService: commitToVi failed");
    }
    m_active = createdCount > 0;
    NC_LOGI("OsdService: elements rebuilt (configured={}, slots used={}/channel, limit={}, main={}x{} sub={}x{})",
            static_cast<int>(m_cfg.elements.size()), usedSlots, maxRegions,
            m_mainRes.width, m_mainRes.height, m_subRes.width, m_subRes.height);
    return true;
}

/**
 * 时间元素创建：千分比→像素，字号取 16 整倍档，画布双钳位。
 * 画布宽按最长一版时间串估：先用当前格式拼一版真时间算字宽。
 *
 * @return nullptr=失败，调用方记日志跳过该元素。
 * @note 要求已持 m_mutex（同 rebuildElementsLocked）
 */
osd::Element* OsdService::createTimeElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId) {
    const ChannelCfg& res = (ch == 0) ? m_mainRes : m_subRes;
    const int W = res.width;
    const int H = res.height;
    if (W <= 0 || H <= 0) {
        return nullptr;
    }
    const int font = osd::fontSizeFromLevel(ec.size, H);
    int x = ec.x * W / 1000;
    int y = ec.y * H / 1000;
    /* 画布宽按最长一版时间串估：先用当前格式拼一版真时间算字宽，
     * 字宽 = 字号/2 × 字单位数（半角字宽是字号一半），再加边缘保护余量。
     * 余量留宽了贴右摆放时可见文字离右边缘远（前后端契约见 kTextEdgePadPx）。
     * 高 = 一行字高 + 上下各留几个像素 */
    const std::string sample = currentTimeString(ec.format, ec.showWeek);
    const int units = charUnits(sample.empty() ? "YYYY-MM-DD HH:MM:SS" : sample);
    int cw = font / 2 * units + osd::kTextEdgePadPx;
    int chh = font + 8;
    clampCanvasLocked(ch, W, H, x, y, cw, chh);

    osd::TimeElementConfig timeCfg;
    timeCfg.at(x, y)
           .withSize(cw, chh)          /* SDK 建画布必须有宽高，缺了直接报 region_size 0 error */
           .withFontSize(font)
           .withColor(osd::Color::White())
           .dateFormat(ec.format == "MM-DD-YYYY" ? osd::DateFormat::MMDDYYYY :
                       ec.format == "Chinese"    ? osd::DateFormat::Chinese :
                                                   osd::DateFormat::YYYYMMDD)
           .showWeek(ec.showWeek);
    return m_engine->createTimeElement(timeCfg, ch, rectId);
}

/**
 * 文本标签创建：同时间的换算规则，画布宽按标签内容字宽估。
 * 空文本在外层已拦（不占槽），这里不再重复判。
 *
 * @return nullptr=失败，调用方记日志跳过该元素。
 * @note 要求已持 m_mutex（同 rebuildElementsLocked）
 */
osd::Element* OsdService::createLabelElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId) {
    const ChannelCfg& res = (ch == 0) ? m_mainRes : m_subRes;
    const int W = res.width;
    const int H = res.height;
    if (W <= 0 || H <= 0) {
        return nullptr;
    }
    const int font = osd::fontSizeFromLevel(ec.size, H);
    int x = ec.x * W / 1000;
    int y = ec.y * H / 1000;
    /* 画布宽 = 字宽 + 边缘保护余量（与时间元素一致，见 createTimeElemLocked） */
    int cw = font / 2 * charUnits(ec.text) + osd::kTextEdgePadPx;
    int chh = font + 8;
    clampCanvasLocked(ch, W, H, x, y, cw, chh);

    osd::TextElementConfig textCfg;
    textCfg.withText(ec.text)
           .at(x, y)
           .withFontSize(font)
           .withColor(osd::Color::White());
    return m_engine->createTextElement(textCfg, ch, rectId);
}

/**
 * 遮盖图形创建：归一化坐标按通道换算，主/子覆盖同一片场景区域。
 * 库内会再对分辨率钳一道，这里先把尺寸钳进 SDK 画布上限，
 * 免得小分辨率通道上画布超限创建失败。
 *
 * @return nullptr=失败，调用方记日志跳过该元素。
 * @note 要求已持 m_mutex（同 rebuildElementsLocked）
 */
osd::Element* OsdService::createShapeElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId) {
    const ChannelCfg& res = (ch == 0) ? m_mainRes : m_subRes;
    const int W = res.width;
    const int H = res.height;
    if (W <= 0 || H <= 0) {
        return nullptr;
    }
    const osd::Color color = osd::parseColorName(ec.color);

    int maxW = W, maxH = H;
    if (m_engine->getMaxRectSize(ch, maxW, maxH) != 0) {
        maxW = W;   /* 查不到上限就用分辨率兜底 */
        maxH = H;
    }
    /* 子通道这里查出来的宽度会是 320（硬件固定，见文件头），
     * 超宽的图形会被钳进 320，属预期行为不是故障 */

    if (ec.type == "rect") {
        int px = ec.x * W / 1000;
        int py = ec.y * H / 1000;
        int pw = ec.w * W / 1000;
        int ph = ec.h * H / 1000;
        clampCanvasLocked(ch, W, H, px, py, pw, ph);
        osd::RectConfig rc;
        rc.at(px, py)
          .withSize(pw, ph)
          .borderWidth(0)
          .withFill(color)
          .withOpacity(ec.opacity);   /* 配置层已钳到 0~100，透传 */
        return m_engine->createRectElement(rc, ch, rectId);
    }
    if (ec.type == "circle") {
        /* 半径相对画面高度换算；钳位保证包围盒（圆心±半径）
         * 既不超画面也不超 SDK 画布上限 */
        int cx = ec.x * W / 1000;
        int cy = ec.y * H / 1000;
        int r = ec.r * H / 1000;
        const int rMax = std::min({W / 2, H / 2, maxW / 2, maxH / 2});
        if (r > rMax) r = rMax;
        if (cx - r < 0) cx = r;
        if (cy - r < 0) cy = r;
        if (cx + r > W) cx = W - r;
        if (cy + r > H) cy = H - r;
        osd::CircleConfig cc;
        cc.at(cx, cy)
          .withRadius(r)
          .borderWidth(0)
          .withFill(color)
          .withOpacity(ec.opacity);
        return m_engine->createCircleElement(cc, ch, rectId);
    }
    if (ec.type == "ellipse") {
        int cx = ec.x * W / 1000;
        int cy = ec.y * H / 1000;
        int rx = ec.w * W / 1000 / 2;
        int ry = ec.h * H / 1000 / 2;
        if (rx > maxW / 2) rx = maxW / 2;
        if (ry > maxH / 2) ry = maxH / 2;
        if (rx > W / 2) rx = W / 2;
        if (ry > H / 2) ry = H / 2;
        if (cx - rx < 0) cx = rx;
        if (cy - ry < 0) cy = ry;
        if (cx + rx > W) cx = W - rx;
        if (cy + ry > H) cy = H - ry;
        osd::EllipseConfig ecfg;
        ecfg.at(cx, cy)
            .withRadius(rx, ry)
            .borderWidth(0)
            .withFill(color)
            .withOpacity(ec.opacity);
        return m_engine->createEllipseElement(ecfg, ch, rectId);
    }
    /* 未知类型配置层已剔除，走不到这里；防御性返回失败 */
    return nullptr;
}

/**
 * 多边形创建：千分比顶点按通道换算成像素，包围盒双钳位后交给引擎。
 *
 * 为什么要在这里钳包围盒：库内只会把画布钳到通道分辨率，不认 SDK 的
 * 画布尺寸上限（子通道宽恒 320），超限创建会失败；钳完包围盒再把落在外面的
 * 顶点拉回盒内，保证库内从顶点重算的画布不会超出钳位结果（形状轻微变形，
 * 和矩形超限被钳小同语义）。
 *
 * @return nullptr=失败，调用方记日志跳过该元素。
 * @note 要求已持 m_mutex（同 rebuildElementsLocked）
 */
osd::Element* OsdService::createPolygonElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId) {
    const ChannelCfg& res = (ch == 0) ? m_mainRes : m_subRes;
    const int W = res.width;
    const int H = res.height;
    if (W <= 0 || H <= 0 || ec.points.size() < 3) {
        return nullptr;
    }

    /* 千分比顶点 → 像素，先钳进画面 */
    std::vector<osd::Point> pts;
    pts.reserve(ec.points.size());
    for (const auto& p : ec.points) {
        osd::Point pt;
        pt.x = p.first * W / 1000;
        pt.y = p.second * H / 1000;
        if (pt.x >= W) pt.x = W - 1;
        if (pt.y >= H) pt.y = H - 1;
        pts.push_back(pt);
    }

    /* 算顶点包围盒，按画布双钳位（分辨率 + SDK 上限）钳出一块合法区域 */
    int bx = pts[0].x, by = pts[0].y, bx2 = pts[0].x, by2 = pts[0].y;
    for (const auto& p : pts) {
        if (p.x < bx) bx = p.x;
        if (p.x > bx2) bx2 = p.x;
        if (p.y < by) by = p.y;
        if (p.y > by2) by2 = p.y;
    }
    int bw = bx2 - bx + 1;
    int bh = by2 - by + 1;
    clampCanvasLocked(ch, W, H, bx, by, bw, bh);

    /* 超出合法区域的顶点拉回盒内（库内按顶点重算画布，不拉回就白钳） */
    for (auto& p : pts) {
        if (p.x < bx) p.x = bx;
        if (p.x > bx + bw - 1) p.x = bx + bw - 1;
        if (p.y < by) p.y = by;
        if (p.y > by + bh - 1) p.y = by + bh - 1;
    }

    osd::PolygonConfig pc;
    pc.withPoints(pts)
      .withFill(osd::parseColorName(ec.color))
      .withOpacity(ec.opacity);   /* 配置层已钳到 0~100，透传 */
    return m_engine->createPolygonElement(pc, ch, rectId);
}

/**
 * 位图创建：千分比位置/尺寸换算后双钳位，路径直接透传给引擎。
 *
 * 文件不在这时读（元素 draw 时才加载）：路径不存在只会让这一个元素创建失败，
 * 不影响其他水印。缺路径/零尺寸在配置规范化时已剔除，这里只做兜底。
 *
 * @return nullptr=失败，调用方记日志跳过该元素。
 * @note 要求已持 m_mutex（同 rebuildElementsLocked）
 */
osd::Element* OsdService::createBitmapElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId) {
    const ChannelCfg& res = (ch == 0) ? m_mainRes : m_subRes;
    const int W = res.width;
    const int H = res.height;
    if (W <= 0 || H <= 0) {
        return nullptr;
    }
    int px = ec.x * W / 1000;
    int py = ec.y * H / 1000;
    int pw = ec.w * W / 1000;
    int ph = ec.h * H / 1000;
    if (pw <= 0 || ph <= 0 || ec.imagePath.empty()) {
        return nullptr;
    }
    clampCanvasLocked(ch, W, H, px, py, pw, ph);

    osd::BitmapConfig bc;
    bc.fromFile(ec.imagePath)
      .at(px, py)
      .withSize(pw, ph)
      .withOpacity(ec.opacity);
    return m_engine->createBitmapElement(bc, ch, rectId);
}

/**
 * 销毁两路通道上的全部元素并清空列表，幂等（元素为空时是空操作）。
 * 销毁后 SDK 里没区域了，画面自然干净，不需要单独清屏。
 *
 * @note 只能在持 m_mutex 且刷新线程已停时调用。
 */
void OsdService::destroyElementsLocked() {
    for (auto* elem : m_allElems) {
        if (elem) {
            m_engine->destroyElement(elem);
        }
    }
    m_allElems.clear();
    /* 时间元素记录里的指针已失效，同步清空，防刷新线程碰野指针 */
    m_timeElems.clear();
}

/**
 * 停掉每秒刷时间的线程：置停止标志 + 唤醒等待 + join 等到线程真正退出。
 *
 * 为什么必须 join 完才返回：调用方接下来就要销毁元素或释放 SDK，
 * 线程没退干净还在画就会碰到已销毁的对象。线程没在跑时直接返回，可重复调。
 */
void OsdService::stopRefreshThread() {
    if (!m_thread.joinable()) {
        return;
    }
    m_stopFlag = true;
    m_cv.notify_all();
    m_thread.join();
}

/**
 * 每秒刷新线程：按各自格式拼一版当前时间喂给每个时间元素，
 * 再遍历全部在屏元素调 update()。
 *
 * 为什么服务自己拼时间：库不读时钟也不重排格式，不喂字符串水印就是空白；
 * 多个时间元素可各带各的格式，逐个拼（同格式重复拼的成本可忽略）。
 * 为什么遍历全部元素：基类 update() 默认空操作，只有时间元素实际重绘，
 * 开销与现在等价；将来加周期性元素不用开新线程。
 *
 * @note 每拍画布操作全程持 m_mutex（start/apply 重建时会先 join 掉本线程再动手，
 *       所以不会和重建抢画布）；等待用单独的 m_cvMutex 避免和画布锁混用；
 *       单拍重绘失败只记警告，下一拍自动重试。
 */
void OsdService::refreshLoop() {
    while (!m_stopFlag) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            /* 库不读时钟也不重排格式：按每个元素自己的格式拼好当前时间喂入 */
            for (auto& slot : m_timeElems) {
                static_cast<osd::TimeElement*>(slot.elem)->setTimeText(
                    currentTimeString(slot.format, slot.showWeek));
            }
            /* 遍历全部在屏元素：基类 update() 返回 0（空操作），
             * 时间元素重绘上屏；单拍失败只记警告，下一拍重试自愈 */
            for (auto* elem : m_allElems) {
                if (elem && elem->update() != 0) {
                    NC_LOGW("OsdService: element update failed, type={}",
                            static_cast<int>(elem->getType()));
                }
            }
        }
        std::unique_lock<std::mutex> lock(m_cvMutex);
        m_cv.wait_for(lock, std::chrono::seconds(1),
                      [this] { return m_stopFlag.load(); });
    }
}

/**
 * 进程退出时的完整收尾：停线程 → 销毁元素 → 释放叠加子系统。
 *
 * 释放后本对象仍可复用：再调 start 会重新初始化（析构之外的场景用得上）。
 * @note 析构函数会调这里，不要在析构链路之外的线程里并发调用。
 */
void OsdService::stop() {
    stopRefreshThread();
    std::lock_guard<std::mutex> lock(m_mutex);
    destroyElementsLocked();
    if (m_sdkInited && m_engine) {
        m_engine->destroy();
        m_sdkInited = false;
    }
    m_active = false;
}

} // namespace mediad
