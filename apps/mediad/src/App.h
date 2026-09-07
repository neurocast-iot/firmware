/**
 * @file App.h
 * @brief mediad 组装根（composition root）：持有全部服务并完成回调布线
 *
 * 服务本身互不相识，依赖注入与编排逻辑全部集中在此，main.cpp 退回纯入口。
 *
 *   ConfigStore ──> CameraService / OsdService / SnapshotService / RecordService
 *                        │
 *   IpcClient ──> CommandRouter ──> 各服务（命令总线）
 *
 * 布线按主题拆分（各自方法内有完整注释）：
 *   wireMediaFileEvents()  文件就绪 → MEDIA_FILE_READY 广播
 *   wireCameraLifecycle()  相机启动/重启编排（所有服务停止→恢复）
 *   wireConfigListener()   CONFIG_UPDATE → 各服务生效编排
 */
#pragma once

#include "config/ConfigStore.h"
#include "services/CameraService.h"
#include "services/OsdService.h"
#include "osd/OsdEngine.h"
#include "pal/osd_backend.h"
#include "services/SnapshotService.h"
#include "services/RecordService.h"
#include "services/LiveStreamService.h"
#include "triggers/trigger_manager.h"
#include "ipc/CommandRouter.h"
#include "ipc/IpcClient.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mediad {

class App {
public:
    App() = default;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /**
     * 初始化：配置加载 → 回调布线 → 启动序列
     * 局部故障不阻断启动（IPC 起不来/相机 init 失败都降级继续跑，
     * 服务永不退出原则），因此无返回值。
     */
    void init(const std::string& configPath);

    /**
     * 优雅退出序列：
     * IPC 停（不再接新命令）→ 录像收卷 → 定时拍照停 → OSD 销毁 → 相机停
     */
    void shutdown();

private:
    void wireFileReadyCallbacks();
    void wireCameraLifecycle();
    void wireConfigListener();
    void startServices();

    /* 触发源工厂：TriggerCfg → Trigger 子类，集中在一处，加新类型只改这里 */
    std::unique_ptr<Trigger> createTrigger(const TriggerCfg& cfg);
    /* 注册单个触发源到 TriggerManager，同时更新 active 计数 */
    void registerSingleTrigger(const TriggerCfg& cfg);
    /* 按当前 active 计数按需启停 JPEG 编码器（拍照原图 + 缩略图） */
    void updateJpegEncoders();

    void initTriggers(const MediadConfig& cfg);
    void applyTriggers(const MediadConfig& cfg);
    bool triggersChanged(const std::vector<TriggerCfg>& oldTriggers,
                         const std::vector<TriggerCfg>& newTriggers);

    /* 声明顺序 = 构造依赖顺序（后者引用前者），勿调整 */
    ConfigStore     m_configStore;
    CameraService   m_cameraService;
    /* OSD 引擎在此组装：平台后端（板上为 anyka 硬件）+ 默认字体路径；
     * 构造早于配置加载，配置里的 font_file 由 OsdService.start() 热更进引擎 */
    OsdService      m_osdService{std::make_unique<osd::OsdEngine>(
                                     pal::CreateOsdBackend(),
                                     "/usr/share/fonts/osd_font_16.bin")};
    SnapshotService m_snapshotService{m_cameraService};
    RecordService   m_recordService{m_cameraService};
    LiveStreamService m_liveService{m_cameraService};
    TriggerManager  m_triggerManager{m_snapshotService, m_recordService};
    CommandRouter   m_router{m_configStore, m_cameraService, m_osdService,
                             m_recordService, m_liveService,
                             m_triggerManager};
    IpcClient       m_ipcClient{m_router};

    /* 当前启用的拍照触发源数量（timer + bluetooth），用于按需启停 VENC JPEG 原图编码器 */
    int m_activeSnapshotTriggers = 0;
    /* 当前启用的录像触发源数量（record）：录像结束要抓缩略图，录像期间缩略图编码器也得开着 */
    int m_activeRecordTriggers = 0;
    /* 当前 triggers 配置（用于增量更新时对比） */
    std::vector<TriggerCfg> m_currentTriggers;

    /* 关停栈：每成功启动一个模块就把关停动作压栈，退出时倒序执行 */
    std::vector<std::function<void()>> m_stopStack;
};

} // namespace mediad
