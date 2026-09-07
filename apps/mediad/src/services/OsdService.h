/**
 * @file OsdService.h
 * @brief OSD 水印服务：时间/文本水印的生命周期管理（对齐拍照/录像服务范式：
 *        start/apply/stop + pauseForRestart/resumeAfterRestart）
 *
 * 基于 nc::osd（OsdEngine 实例引擎 + Element 元素模型），引擎由 App 构造注入，
 * 内部持有平台后端（板上为 anyka 硬件后端）；
 * 使用序列取证自 gw_av100 iot_live-cpp OsdManager：
 *   setLogCallback → init → setChannelResolution(每通道)
 *   → createTimeElement/createTextElement(channel, rect_id 0-3)
 *   → commitToVi → 秒级 setTimeText + redrawElement(时间元素刷新)
 *   → destroyElement + destroy
 *
 * 说明：
 *   - 主/子通道都画水印（录像走主通道、拍照走子通道，两者都带水印）
 *   - rect_id 按配置数组顺序动态分配，不再区分时间/文本/图形固定槽位；
 *     每通道能放几个问平台后端（anyka 芯片固定 4，其他平台可能更多），
 *     超量元素截断并告警；接新平台容量变大时配置不用改、代码也不用改
 *   - 时间元素契约：库自己不读时钟，由本服务每秒生成时间字符串，先调
 *     setTimeText() 喂进去再重绘；不喂时间，水印就是空白。刷新线程遍历
 *     全部在屏元素调 update()（基类默认空操作，只有时间元素实际重绘），
 *     将来加周期性元素不用开新线程；开关关闭或时间元素被移除时线程停止
 *   - 跨分辨率：配置存千分比意图，本服务按各通道实际分辨率换算像素，
 *     字号对齐 16 的倍数，画布尺寸双钳位（通道分辨率 + SDK max_rect）
 *   - 字体文件：配置里的 font_file 在 start() 时交给引擎，后端在每次画字符串前
 *     自动重设（原 refreshGlobalFont 约束下沉），热更路径由 osdCfgChanged 触发重建
 *   - 必须在相机 start 成功之后调用 start()（OSD 依赖 VI 通道就绪）；
 *     SDK init 与 VI 会话绑定，相机重启前必须销毁，重启成功后重新初始化
 *
 * 线程模型：start/apply/stop 在 IPC 命令线程或主线程串行调用；
 * 秒级刷新线程只碰元素指针，用 m_mutex 与 apply 的重建互斥。
 */
#pragma once

#include "config/ConfigStore.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/* 前向声明，避免头文件外泄 osd 依赖 */
namespace osd { class Element; class TimeElement; class OsdEngine; }

namespace mediad {

class OsdService {
public:
    /**
     * 构造：注入 OSD 引擎（内含平台后端）
     *
     * 引擎由 App 组装（pal::CreateOsdBackend() + 默认字体路径），
     * 服务独占所有权；构造在配置加载之前，配置里的字体路径在 start() 时应用。
     */
    explicit OsdService(std::unique_ptr<osd::OsdEngine> engine);
    ~OsdService();

    OsdService(const OsdService&) = delete;
    OsdService& operator=(const OsdService&) = delete;

    /**
     * 启动 OSD（相机 start 成功后调用）
     *
     * cfg.enabled=false 时不初始化 SDK，直接返回 true（视为按配置生效）。
     *
     * @param cfg  OSD 配置
     * @param main 主通道当前分辨率（用于画布钳位）
     * @param sub  子通道当前分辨率
     */
    bool start(const OsdCfg& cfg, const ChannelCfg& main, const ChannelCfg& sub);

    /**
     * 应用新配置 / 分辨率变更后重绑（enabled 开关热更入口）
     *
     * 重建所有元素（销毁旧元素 → 更新通道分辨率 → 按新配置创建）。
     * 处理 enabled 开关翻转：false→true 时补做初始化，true→false 时
     * 清空在屏元素、停掉刷新线程并清屏（SDK 保留，再开秒回）。
     */
    bool apply(const OsdCfg& cfg, const ChannelCfg& main, const ChannelCfg& sub);

    /**
     * 相机重启前清场（对齐原工程 OsdManager 的 CameraWillRestart 序列）
     *
     * 停刷新线程、销毁全部元素并释放 SDK：ak_osd 的 init 绑定 VI 会话，
     * 相机重启后旧上下文失效，必须销毁强制下次重走 init。
     */
    void pauseForRestart();

    /**
     * 相机重启成功后恢复（重新初始化 SDK + 按当前配置重建元素）
     *
     * 重启失败时不调用：保持暂停态，待相机重新拉起后由 onStarted 回调
     * 走 start() 统一恢复。
     */
    bool resumeAfterRestart(const OsdCfg& cfg, const ChannelCfg& main, const ChannelCfg& sub);

    /** 停止刷新线程、销毁元素并释放 SDK（幂等，进程退出路径） */
    void stop();

    bool isActive() const { return m_active; }

private:
    /* 以下方法要求调用方已持有 m_mutex */
    void destroyElementsLocked();
    bool rebuildElementsLocked();

    /* 单元素创建分支（按类型分发）：配置里的千分比在这里换成像素，
     * 画布双钳位后交给引擎建元素；失败返回 nullptr，调用方跳过该元素。
     * 要求已持 m_mutex（同 rebuildElementsLocked） */
    osd::Element* createTimeElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId);
    osd::Element* createLabelElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId);
    osd::Element* createShapeElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId);
    osd::Element* createPolygonElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId);
    osd::Element* createBitmapElemLocked(const OsdCfg::ElemCfg& ec, int ch, int rectId);

    /* 画布双钳位（要问引擎该通道的画布上限，所以是成员方法而非自由函数）；
     * 要求已持 m_mutex（同 rebuildElementsLocked） */
    bool clampCanvasLocked(int channel, int videoW, int videoH,
                           int& x, int& y, int& w, int& h);

    /* 刷新线程控制：调用方不得持有 m_mutex（join 会与线程内的取锁互斥死锁） */
    void stopRefreshThread();

    void refreshLoop();   ///< 秒级：拼时间串喂时间元素 → 遍历全部元素 update()

    mutable std::mutex m_mutex;
    std::unique_ptr<osd::OsdEngine> m_engine;   ///< OSD 引擎（构造注入，独占所有权）
    OsdCfg     m_cfg;
    ChannelCfg m_mainRes = {};
    ChannelCfg m_subRes  = {};

    /* 时间元素记录：元素指针 + 各自的时间格式（多个时间元素可各带各的格式，
     * 刷新线程按各自格式拼字符串喂入）。指针归引擎创建/销毁 */
    struct TimeElemSlot {
        osd::Element* elem = nullptr;
        std::string format;                 ///< 该元素的时间格式（来自配置）
        bool showWeek = false;
    };
    std::vector<TimeElemSlot> m_timeElems;   ///< 在屏时间元素（可能多个）
    std::vector<osd::Element*> m_allElems;   ///< 全部在屏元素扁平列表（刷新线程遍历用）
    std::string m_curFontFile;               ///< 已应用的字体路径（字体变化只换字体不重建元素）

    bool m_sdkInited = false;             ///< 引擎 init 是否已做
    /* 刷新线程是否启动看 m_timeElems 是否为空，不再单独记标志 */
    std::atomic<bool> m_active{false};    ///< 水印是否在屏

    std::thread             m_thread;
    std::atomic<bool>       m_stopFlag{false};
    std::mutex              m_cvMutex;
    std::condition_variable m_cv;
};

} // namespace mediad
