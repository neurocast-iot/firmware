/**
 * @file App.cpp
 * @brief mediad 组装根实现：布线逻辑从 main.cpp 原样迁入，按主题分方法
 */
#include "App.h"
#include "ipc/MediadMessages.h"
#include "triggers/timer_trigger.h"
#include "triggers/record_trigger.h"
#ifdef MEDIAD_BT_SUPPORT
#include "triggers/bluetooth_trigger.h"
#endif

#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"

#include "cJSON.h"

#include <ctime>

namespace mediad {

void App::init(const std::string& configPath) {
    /* 配置：文件缺失/损坏用默认值继续跑（兜底原则） */
    m_configStore.load(configPath);

    wireFileReadyCallbacks();
    wireCameraLifecycle();
    wireConfigListener();
    startServices();

    NC_LOGI("mediad running (config={})", configPath.c_str());
}

void App::shutdown() {
    NC_LOGI("mediad shutting down...");
    for (auto it = m_stopStack.rbegin(); it != m_stopStack.rend(); ++it) {
        (*it)();
    }
    NC_LOGI("mediad exited");
}

/* 文件就绪回调连线：拍照由触发源通知，录像由 RecordService 通知，
 * 两条路径最终都走 MEDIA_FILE_READY 事件广播给 iot_agent */
void App::wireFileReadyCallbacks() {
    /* 拍照通知由触发源发出（谁产出文件谁通知），不再由 SnapshotService 统一通知 */
    m_triggerManager.setFileReadyCallback(
        [this](const std::string& triggerType, const std::string& path, int64_t size,
               const std::string& thumbPath, int64_t thumbSize) {
            m_ipcClient.sendEvent(libmq::IpcMessageType::MEDIA_FILE_READY,
                                  buildFileReadyPayload(triggerType, "image", path, size,
                                                        thumbPath, thumbSize, std::time(nullptr)));
        });
    /* 录像通知由 RecordService 发出（收卷时已抓好视频缩略图，与拍照对称）
     * startTimeSec/durationMs 一并带给云端：服务器回放选文件时用它算覆盖范围 */
    m_recordService.setFileReadyCallback(
        [this](const std::string& triggerType, const std::string& path, int64_t size,
               uint64_t durationMs, uint64_t startTimeSec,
               const std::string& thumbPath, int64_t thumbSize) {
            m_ipcClient.sendEvent(libmq::IpcMessageType::MEDIA_FILE_READY,
                                  buildFileReadyPayload(triggerType, "video", path, size,
                                                        thumbPath, thumbSize, std::time(nullptr),
                                                        startTimeSec, durationMs / 1000));
        });
}

/* 相机生命周期编排（启动成功 + 分辨率热更重启） */
void App::wireCameraLifecycle() {
    /* 相机启动成功（重试线程回调）→ OSD 上屏 + 常态录像起录 + 拍照解除暂停
     * （重启失败后相机重新拉起时，各服务在此统一恢复）+ 状态广播 */
    m_cameraService.setOnStarted([this]() {
        const MediadConfig cfg = m_configStore.get();
        const camera::CameraConfig& actual = m_cameraService.device()->config();
        /* OSD 按配置上屏：开关开着就在相机就绪后默认启动（依赖 VI 通道就绪） */
        m_osdService.start(cfg.osd, toChannelCfg(actual.mainChannel()),
                           toChannelCfg(actual.subChannel()));
        /* 相机就绪后才能订阅主通道帧：scheduled 开关开着就从这里起录 */
        m_recordService.onCameraStarted();
        m_snapshotService.resumeAfterRestart();
        /* 直播服务在此拉起（依赖 device 就绪：建会话时要取实际生效分辨率）；
         * 重复调用幂等（内部判 already started），重启后重新拉起无副作用 */
        m_liveService.start(cfg.deviceId, cfg.live);
        m_liveService.resumeAfterRestart();
        updateJpegEncoders();
        m_ipcClient.sendEvent(libmq::IpcMessageType::MEDIA_STATE_CHANGED, "{\"camera\":\"running\"}");
    });

    /* 分辨率热更编排：相机重启前所有服务必须停止，重启成功后
     * 已开启的服务自动恢复（各服务只与相机有时序约束，彼此无关）：
     * 重启前：录像收卷 + OSD 销毁 SDK + 拍照排空（旧 VI 上下文即将失效）
     * 重启后：成功才重建 OSD（新实际分辨率）+ 恢复录像/拍照；
     *         失败则保持暂停态，待相机重新拉起后由 onStarted 回调恢复 */
    m_cameraService.setRestartHooks(
        [this]() {
            /* 重启前收卷：旧 VI 上下文即将失效，不收卷文件就是坏的 */
            m_recordService.pauseForRestart();
            m_osdService.pauseForRestart();
            m_snapshotService.pauseForRestart();
            m_triggerManager.pauseForRestart();
            /* 结束所有直播会话（发 bye/停推流，退订主通道，观看端自行重连） */
            m_liveService.pauseForRestart();
        },
        [this]() {
            if (!m_cameraService.isRunning()) {
                NC_LOGW("camera restart failed, osd/record/snapshot stay paused");
                return;
            }
            const MediadConfig cfg = m_configStore.get();
            const camera::CameraConfig& actual = m_cameraService.device()->config();
            m_osdService.resumeAfterRestart(cfg.osd, toChannelCfg(actual.mainChannel()),
                                            toChannelCfg(actual.subChannel()));
            m_recordService.resumeAfterRestart();
            m_snapshotService.resumeAfterRestart();
            m_triggerManager.resumeAfterRestart();
            m_liveService.resumeAfterRestart();
            updateJpegEncoders();
        });
}

/* 配置热更联动：IPC CONFIG_UPDATE → ConfigStore 落盘 → 此处编排各服务生效 */
void App::wireConfigListener() {
    m_configStore.addListener([this](const MediadConfig& oldCfg, const MediadConfig& newCfg) {
        bool restarted = false;

        /* 相机段：分辨率变化走重启编排（钩子自动处理录像/OSD/拍照） */
        std::string errMsg;
        if (m_cameraService.isRunning() &&
            !m_cameraService.applyCameraConfig(newCfg.camera, restarted, errMsg)) {
            NC_LOGE("config apply: camera failed: {}", errMsg.c_str());
        }

        /* OSD 段：未发生相机重启且显示参数真变了才重建（避免无关配置
         * 更新导致水印闪烁）；enabled 开关热更也走这里；重启路径由
         * restartDone 钩子无条件重建 */
        if (!restarted && m_cameraService.isRunning() &&
            osdCfgChanged(oldCfg.osd, newCfg.osd)) {
            const camera::CameraConfig& actual = m_cameraService.device()->config();
            m_osdService.apply(newCfg.osd, toChannelCfg(actual.mainChannel()),
                               toChannelCfg(actual.subChannel()));
        }
        m_recordService.apply(newCfg.record);

        m_snapshotService.apply(newCfg.snapshot);

        /* triggers 段：配置变了就清空重建 */
        if (triggersChanged(oldCfg.triggers, newCfg.triggers)) {
            applyTriggers(newCfg);
        }

        /* live 段：设备 ID/enabled/broker 变化整体重启，其余字段热更
         * （设备 ID 首次下发就走这条路径：空 → 实值 → 连信令 broker） */
        m_liveService.apply(newCfg.deviceId, newCfg.live);

        if (oldCfg.ipc.iotAgentEndpoint != newCfg.ipc.iotAgentEndpoint) {
            NC_LOGW("config apply: iot_agent endpoint changed, effective after restart");
        }
        /* 配置已生效，ACK 由 ConfigSyncClient 自动发送 */
    });
}

/* 从配置创建触发源并注册到 TriggerManager */
void App::initTriggers(const MediadConfig& cfg) {
    m_activeSnapshotTriggers = 0;
    m_activeRecordTriggers = 0;

    for (const auto& tcfg : cfg.triggers) {
        registerSingleTrigger(tcfg);
    }

    NC_LOGI("App: {} triggers initialized ({} snapshot, {} record active)",
             cfg.triggers.size(), m_activeSnapshotTriggers, m_activeRecordTriggers);

    m_currentTriggers = cfg.triggers;
    updateJpegEncoders();
}

/* 配置热更时增量更新触发源：只更新变化的，没变的不受影响 */
void App::applyTriggers(const MediadConfig& cfg) {
    NC_LOGI("App: applying triggers configuration change (incremental)");
    
    const auto& newTriggers = cfg.triggers;
    
    /* 第一步：找出需要删除的 triggers（在旧配置里但不在新配置里） */
    std::vector<std::string> toRemove;
    for (const auto& oldCfg : m_currentTriggers) {
        bool found = false;
        for (const auto& newCfg : newTriggers) {
            if (oldCfg.id == newCfg.id) {
                found = true;
                break;
            }
        }
        if (!found) {
            toRemove.push_back(oldCfg.id);
        }
    }
    
    /* 第二步：找出需要新增或更新的 triggers */
    std::vector<const TriggerCfg*> toAddOrUpdate;
    for (const auto& newCfg : newTriggers) {
        bool found = false;
        for (const auto& oldCfg : m_currentTriggers) {
            if (oldCfg.id == newCfg.id) {
                found = true;
                if (oldCfg != newCfg) {
                    toAddOrUpdate.push_back(&newCfg);
                }
                /* 配置没变，跳过 */
                break;
            }
        }
        if (!found) {
            /* 新增的 trigger */
            toAddOrUpdate.push_back(&newCfg);
        }
    }
    
    /* 如果没有变化，直接返回 */
    if (toRemove.empty() && toAddOrUpdate.empty()) {
        NC_LOGI("App: no triggers changed, skip update");
        return;
    }
    
    /* 第三步：执行删除 */
    for (const auto& id : toRemove) {
        NC_LOGI("App: removing trigger [{}]", id.c_str());
        m_triggerManager.removeTrigger(id.c_str());
    }
    
    /* 第四步：执行新增/更新
     * 只删除/创建变化的 triggers，没变的保持不动 */
    
    bool hasSnapshotChange = false;
    bool hasRecordChange = false;
    
    for (const auto* cfgPtr : toAddOrUpdate) {
        if (cfgPtr->type == "timer" || cfgPtr->type == "bluetooth") {
            hasSnapshotChange = true;
        } else if (cfgPtr->type == "record") {
            hasRecordChange = true;
        }
    }
    
    /* 如果有 record trigger 变化，需要重启录像 */
    if (hasRecordChange) {
        NC_LOGI("App: record trigger changed, will restart recording");
    }
    
    /* 如果有 snapshot trigger 变化，需要重启拍照编码器 */
    if (hasSnapshotChange) {
        NC_LOGI("App: snapshot trigger changed, will restart snapshot encoder");
        if (m_cameraService.isRunning()) {
            m_cameraService.device()->stopSnapshot();
            m_cameraService.device()->stopThumbnail();
        }
    }
    
    /* 只删除变化的 triggers（没变的保持不动） */
    for (const auto* cfgPtr : toAddOrUpdate) {
        /* 先删除旧的（如果存在） */
        m_triggerManager.removeTrigger(cfgPtr->id.c_str());
    }
    
    /* 重新计算 active counts（只计算没变的 + 新增/更新的） */
    m_activeSnapshotTriggers = 0;
    m_activeRecordTriggers = 0;
    
    /* 用工厂方法创建新增/更新的 triggers */
    for (const auto* cfgPtr : toAddOrUpdate) {
        registerSingleTrigger(*cfgPtr);
    }
    
    /* 加上没变的 triggers 的 counts */
    for (const auto& oldCfg : m_currentTriggers) {
        bool isChanged = false;
        for (const auto* cfgPtr : toAddOrUpdate) {
            if (cfgPtr->id == oldCfg.id) { isChanged = true; break; }
        }
        if (!isChanged && oldCfg.enabled) {
            if (oldCfg.type == "timer" || oldCfg.type == "bluetooth") m_activeSnapshotTriggers++;
            else if (oldCfg.type == "record") m_activeRecordTriggers++;
        }
    }

    updateJpegEncoders();
    
    /* 更新当前配置缓存 */
    m_currentTriggers = newTriggers;
    
    NC_LOGI("App: triggers updated (removed={}, added/updated={}, snapshotRestart={}, recordRestart={}, activeSnapshot={}, activeRecord={})",
            toRemove.size(), toAddOrUpdate.size(), hasSnapshotChange, hasRecordChange,
            m_activeSnapshotTriggers, m_activeRecordTriggers);
}

/* 比较两个 triggers 配置是否相同（直接走 TriggerCfg::operator==） */
bool App::triggersChanged(const std::vector<TriggerCfg>& oldTriggers,
                          const std::vector<TriggerCfg>& newTriggers) {
    return oldTriggers != newTriggers;
}

/* ---- 触发源工厂：TriggerCfg → Trigger 子类，加新类型只改这一处 ---- */
std::unique_ptr<Trigger> App::createTrigger(const TriggerCfg& tcfg) {
    if (tcfg.type == "timer") {
        TimerTrigger::Config tc;
        tc.id = tcfg.id;
        tc.enabled = tcfg.enabled;
        tc.priority = tcfg.priority;
        tc.intervalSec = tcfg.intervalSec;
        tc.burstCount = tcfg.burstCount;
        tc.burstIntervalMs = tcfg.burstIntervalMs;
        tc.allDay = tcfg.allDay;
        if (!tcfg.scheduleStart.empty() && !tcfg.scheduleEnd.empty()) {
            tc.schedule.load(tcfg.scheduleStart, tcfg.scheduleEnd, tcfg.scheduleDays);
            tc.scheduleStart = tcfg.scheduleStart;
            tc.scheduleEnd = tcfg.scheduleEnd;
            tc.scheduleDays = tcfg.scheduleDays;
        }
        return std::unique_ptr<Trigger>(new TimerTrigger(tc));
    }
    if (tcfg.type == "record") {
        RecordTrigger::Config rc;
        rc.id = tcfg.id;
        rc.priority = tcfg.priority;
        rc.allDay = tcfg.allDay;
        if (!tcfg.scheduleStart.empty() && !tcfg.scheduleEnd.empty()) {
            rc.startHour = std::stoi(tcfg.scheduleStart.substr(0, 2));
            rc.startMin  = std::stoi(tcfg.scheduleStart.substr(3, 2));
            rc.endHour   = std::stoi(tcfg.scheduleEnd.substr(0, 2));
            rc.endMin    = std::stoi(tcfg.scheduleEnd.substr(3, 2));
        }
        rc.weekdays = tcfg.scheduleDays;
        return std::unique_ptr<Trigger>(new RecordTrigger(rc));
    }
    if (tcfg.type == "bluetooth") {
#ifdef MEDIAD_BT_SUPPORT
        BluetoothTrigger::Config bc;
        bc.id = tcfg.id;
        bc.priority = tcfg.priority;
        bc.burstCount = tcfg.burstCount;
        bc.burstIntervalMs = tcfg.burstIntervalMs;
        return std::unique_ptr<Trigger>(new BluetoothTrigger(bc));
#else
        NC_LOGW("App: bluetooth trigger [{}] ignored (built without MEDIAD_BT_SUPPORT)",
                tcfg.id.c_str());
#endif
    }
    NC_LOGW("App: unknown trigger type [{}], skip", tcfg.type.c_str());
    return nullptr;
}

/* 注册单个触发源到 TriggerManager，同时更新 active 计数 */
void App::registerSingleTrigger(const TriggerCfg& tcfg) {
    auto trigger = createTrigger(tcfg);
    if (!trigger) return;
    m_triggerManager.addTrigger(std::move(trigger));
    if (tcfg.enabled) {
        m_triggerManager.enableTrigger(tcfg.id.c_str());
        if (tcfg.type == "timer" || tcfg.type == "bluetooth") {
            m_activeSnapshotTriggers++;
        } else if (tcfg.type == "record") {
            m_activeRecordTriggers++;
        }
    }
}

/* 按当前 active 计数按需启停 JPEG 编码器（拍照原图 + 缩略图） */
void App::updateJpegEncoders() {
    if (!m_cameraService.isRunning()) return;
    if (m_activeSnapshotTriggers > 0) {
        m_cameraService.device()->startSnapshot();
    } else {
        m_cameraService.device()->stopSnapshot();
    }
    const MediadConfig cfgNow = m_configStore.get();
    if (cfgNow.thumbnail.enabled &&
        (m_activeSnapshotTriggers + m_activeRecordTriggers) > 0) {
        m_cameraService.device()->startThumbnail(
            cfgNow.thumbnail.width, cfgNow.thumbnail.height);
    } else {
        m_cameraService.device()->stopThumbnail();
    }
}

/* 启动序列：定时任务 → IPC → 相机异步启动 */
void App::startServices() {
    MediadConfig cfg = m_configStore.get();

    m_snapshotService.start(cfg.snapshot);
    m_stopStack.push_back([this] { m_snapshotService.stop(); });

    m_recordService.start(cfg.record);
    /* 相机此刻还没起，真正开录由 onCameraStarted 触发 */
    m_stopStack.push_back([this] { m_recordService.stop(); });

    initTriggers(cfg);
    m_stopStack.push_back([this] { m_triggerManager.disableAll(); });

    if (m_ipcClient.start(cfg.ipc.iotAgentEndpoint)) {
        m_stopStack.push_back([this] { m_ipcClient.stop(); });
        m_ipcClient.setConfigApplyCallback(
            [this](uint64_t version, const std::string& config) -> bool {
                NC_LOGI("[ConfigSync] applying config version={}", version);
                std::string errMsg;
                bool ok = m_configStore.applyUpdate(config, errMsg);
                if (!ok) {
                    NC_LOGE("[ConfigSync] apply failed: {}", errMsg.c_str());
                }
                return ok;
            });
    } else {
        NC_LOGE("mediad: IPC start failed, running without IPC");
    }

    if (m_cameraService.init(cfg.camera)) {
        if (cfg.camera.autoStart) {
            m_cameraService.startAsync();
        } else {
            NC_LOGI("mediad: camera auto_start disabled");
        }
    } else {
        NC_LOGE("mediad: camera init failed, media functions degraded");
    }
    m_stopStack.push_back([this] { m_cameraService.stop(); });

    m_liveService.start(cfg.deviceId, cfg.live);
    m_stopStack.push_back([this] { m_liveService.stop(); });

    /* OSD 由 wireCameraLifecycle 的 onStarted 回调在相机就绪后启动 */
    m_stopStack.push_back([this] { m_osdService.stop(); });

    m_ipcClient.notifyStarted(0);
}

} // namespace mediad
