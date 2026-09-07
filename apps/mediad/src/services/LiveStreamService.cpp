/**
 * @file LiveStreamService.cpp
 * @brief 实时视频服务实现（P2P / SFU 状态机 + 信令工作线程）
 *
 * 锁序约定（防死锁）：
 *   m_mutex（状态机大锁）→ 不得在持有期间等待 metaRTC 回调线程持有的锁；
 *   m_sidMutex（候选回调小锁）只做 sid 拷贝，绝不嵌套其他锁；
 *   信令 connect（阻塞可达 10s）在工作线程持锁之外执行，避免卡死
 *   IPC 线程的 status()/startManualPush() 调用。
 */
#include "services/LiveStreamService.h"
#include "services/CameraService.h"

#include "nc/common/log_utils.h"

#include <chrono>
#include <unistd.h>

#include "cJSON.h"

namespace mediad {

/* 工作线程节拍：连接轮询 / 超时判定 / 心跳清理的最大延迟 */
static const int kTickIntervalMs = 500;
/* 信令 broker 重连间隔（4G 开机网络未就绪时的退避周期） */
static const int kSigRetryMs = 5000;

LiveStreamService::~LiveStreamService() {
    stop();
}

int64_t LiveStreamService::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** 内部运行状态名（idle/p2p/sfu），用于日志 */
const char* LiveStreamService::stateName(Mode m) {
    switch (m) {
        case Mode::P2P:   return "p2p";
        case Mode::SFU:   return "sfu";
        default:          return "idle";
    }
}

/** 推流模式名（p2p/p2p_host/sfu），用于日志 */
const char* LiveStreamService::pushModeName(PushMode p) {
    switch (p) {
        case PushMode::P2P:      return "p2p";
        case PushMode::P2P_Host: return "p2p_host";
        case PushMode::SFU:      return "sfu";
        default:                 return "unknown";
    }
}

/* ==================== 生命周期 ==================== */

void LiveStreamService::start(const std::string& devId, const LiveCfg& cfg) {
    if (m_thread.joinable()) {
        NC_LOGW("LiveStreamService: already started");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_devId = devId;
        m_cfg   = cfg;
    }
    if (!cfg.enabled) {
        NC_LOGI("LiveStreamService: disabled by config");
        return;
    }
    /* 设备 ID 由 IoT 进程经 CONFIG_UPDATE 下发：首次开机时可能还没到，
     * 此时不连 broker（否则订阅 p2p//offer 这种畸形 topic）；下发后
     * apply() 检出 devId 变化会整体重启本服务 */
    if (devId.empty()) {
        NC_LOGW("LiveStreamService: device id empty, waiting for IoT process to provision it");
        return;
    }

    /* 设备端固定 pusher 角色：收 offer/cand-viewer/bye/alive */
    m_signaling.reset(new rtc::P2PSignaling(
        cfg.mqttSignaling.url, devId, "pusher",
        cfg.mqttSignaling.username, cfg.mqttSignaling.password));
    m_signaling->setMessageHandler(
        [this](const std::string& type, const std::string& sid, const std::string& body) {
            /* paho 回调线程：只入队（回调内同步 publish 会与 PUBACK 死锁） */
            SigMsg msg;
            msg.type = type;
            msg.sid  = sid;
            msg.body = body;
            /* offer 消息 body 是完整 JSON，需解析出 sdp 和 mode */
            if (type == "offer") {
                cJSON* root = cJSON_Parse(body.c_str());
                if (root) {
                    cJSON* jsdp = cJSON_GetObjectItem(root, "sdp");
                    if (jsdp && cJSON_IsString(jsdp) && jsdp->valuestring) {
                        msg.body = jsdp->valuestring;  /* body 只存 SDP，供 handleOfferP2P 使用 */
                    }
                    cJSON* jmode = cJSON_GetObjectItem(root, "mode");
                    if (jmode && cJSON_IsString(jmode) && jmode->valuestring) {
                        msg.mode = jmode->valuestring;
                    }
                    cJSON_Delete(root);
                }
            }
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_queue.push_back(std::move(msg));
            }
            m_queueCv.notify_one();
        });

    /* PLI 等 RTC 关键帧请求（metaRTC 回调线程）直接接到相机 IDR 请求，
     * 不经工作线程转发：requestKeyFrame 是 SDK 异步请求，无状态依赖 */
    m_session.setKeyFrameHandler([this]() { requestKeyFrame(); });

    /* 本端 ICE 候选外发（metaRTC gather 线程）：只取 m_sidMutex 拷贝 sid，
     * publish 由 P2PSignaling 内部互斥保护 */
    m_session.setCandidateHandler([this](const std::string& cand) {
        std::string sid;
        {
            std::lock_guard<std::mutex> lock(m_sidMutex);
            sid = m_p2pSid;
        }
        if (!sid.empty() && m_signaling) m_signaling->sendCandidate(sid, cand);
    });

    m_stopFlag.store(false);
    m_thread = std::thread(&LiveStreamService::workerLoop, this);
    NC_LOGI("LiveStreamService: started (devId={} broker={} p2pTimeout={}s relayIdle={}s)",
            devId.c_str(), cfg.mqttSignaling.url.c_str(),
            cfg.p2pTimeoutSec, cfg.relayIdleSec);
}

void LiveStreamService::apply(const std::string& devId, const LiveCfg& cfg) {
    bool needRestart;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        /* 检测信令/服务开关变化 → 需要整体重启 */
        needRestart = cfg.enabled                != m_cfg.enabled ||
                      devId                      != m_devId       ||
                      cfg.mqttSignaling.url      != m_cfg.mqttSignaling.url;
        
        /* 检测推流相关配置变化（ICE/SRS/推流模板）→ 停所有推流，下次建会话用新配置 */
        bool streamingConfigChanged = cfg.srs.whipUrlTemplate != m_cfg.srs.whipUrlTemplate ||
                                 cfg.ice.host            != m_cfg.ice.host            ||
                                 cfg.ice.port            != m_cfg.ice.port            ||
                                 cfg.ice.username        != m_cfg.ice.username        ||
                                 cfg.ice.password        != m_cfg.ice.password;
        
        if (streamingConfigChanged && m_mode != Mode::Idle) {
            /* P2P 和 SFU 都停：P2P 下次 offer 用新 ICE；SFU 下次 push 用新 WHIP/ICE */
            NC_LOGI("LiveStreamService: streaming config changed, stopping all sessions");
            teardownAllLocked(false);
            m_cfg = cfg;
            return;
        }
        
        if (!needRestart) {
            /* 超时/SRS 地址/ICE 凭证热更：下次建会话/切 SFU 时生效 */
            m_cfg = cfg;
            NC_LOGI("LiveStreamService: config applied (hot)");
            return;
        }
    }
    NC_LOGI("LiveStreamService: config requires restart (enabled/device id/broker changed)");
    stop();
    start(devId, cfg);
}

void LiveStreamService::stop() {
    if (m_thread.joinable()) {
        m_stopFlag.store(true);
        m_queueCv.notify_one();
        m_thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        teardownAllLocked(true);
    }
    if (m_signaling) {
        m_signaling->disconnect();
        m_signaling.reset();
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }
}

void LiveStreamService::pauseForRestart() {
    m_paused.store(true);
    std::lock_guard<std::mutex> lock(m_mutex);
    teardownAllLocked(true);
    NC_LOGI("LiveStreamService: paused for camera restart");
}

void LiveStreamService::resumeAfterRestart() {
    /* 无动作恢复：观看端收 bye 后自行重连，新 offer 走全新会话
     * （届时从 device()->config() 取重启后的实际分辨率） */
    m_paused.store(false);
    NC_LOGI("LiveStreamService: resumed after camera restart");
}

/* ==================== IPC 命令入口 ==================== */

/* RPC 调用：固定走 SFU 模式，直接起 WHIP 推流
 * P2P 模式由观看端通过 MQTT 信令发 offer 触发，不走此接口
 * 同模式同 token → 跳过；同模式不同 token → 重启推流；不同模式 → 停旧起新 */
LiveStreamService::ManualPushError LiveStreamService::startManualPush(
        const std::string& accessToken) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cfg.enabled) {
        return ManualPushError::Disabled;
    }
    if (m_paused.load()) {
        return ManualPushError::Busy;
    }
    /* 已是 SFU 推流且 token 一致：不重复推流（mode==SFU 时 pushMode 必为 SFU） */
    if (m_mode == Mode::SFU && m_pusher.isPushing() && m_accessToken == accessToken) {
        NC_LOGI("LiveStreamService: already in SFU mode with same token, skip");
        return ManualPushError::Ok;
    }
    /* 不同模式或不同 token：停当前推流，起新推流 */
    if (m_mode != Mode::Idle) {
        /* 通知被踢掉的观看者，告诉原因（前端收到后会立刻改走 WHEP 拉流） */
        if (m_signaling) {
            if (m_mode == Mode::P2P && !m_p2pSid.empty()) {
                m_signaling->sendBye(m_p2pSid, "sfu_active");
            }
            for (const auto& kv : m_sfuViewers) {
                m_signaling->sendBye(kv.first, "sfu_active");
            }
        }
        teardownAllLocked(false);
    }
    m_accessToken = accessToken;
    if (!ensureWhipPushing()) {
        return ManualPushError::Internal;
    }
    m_mode = Mode::SFU;
    m_pushMode = PushMode::SFU;
    m_manualPush = true;
    NC_LOGI("LiveStreamService: SFU manual push started -> {}",
            m_cfg.whipUrl(m_devId, m_accessToken).c_str());
    return ManualPushError::Ok;
}

LiveStreamService::ManualPushError LiveStreamService::stopManualPush(
        const std::string& accessToken) {
    std::lock_guard<std::mutex> lock(m_mutex);
    /* 没有推流：直接返回成功 */
    if (m_mode == Mode::Idle) {
        NC_LOGI("LiveStreamService: stopManualPush called but already idle");
        return ManualPushError::Ok;
    }
    /* SFU 模式需要校验 token：防止旧页面的 stop 误停新页面的推流 */
    if (m_mode == Mode::SFU && accessToken != m_accessToken) {
        NC_LOGW("LiveStreamService: stop rejected, token mismatch (current={} request={})",
                m_accessToken.c_str(), accessToken.c_str());
        return ManualPushError::TokenMismatch;
    }
    /* 停止当前推流（P2P 或 SFU） */
    m_manualPush = false;
    const char* prevMode = stateName(m_mode);  /* 先记下当前模式 */
    teardownAllLocked(false);
    NC_LOGI("LiveStreamService: manual push stopped (was {} mode) -> idle", prevMode);
    return ManualPushError::Ok;
}

LiveStreamService::Status LiveStreamService::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Status st;
    st.mode       = stateName(m_mode);
    /* P2P 已连上才算 1 人，协商中不算 */
    st.viewers    = (m_mode == Mode::P2P && m_p2pConnected) ? 1 : (int)m_sfuViewers.size();
    st.manualPush = m_manualPush;
    return st;
}

/* ==================== 工作线程 ==================== */

void LiveStreamService::workerLoop() {
    int64_t lastConnAttempt = 0;

    while (!m_stopFlag.load()) {
        /* 取一条信令（无消息时最多等一个节拍，保证 tick 周期性执行） */
        SigMsg msg;
        bool hasMsg = false;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait_for(lock, std::chrono::milliseconds(kTickIntervalMs),
                               [this] { return m_stopFlag.load() || !m_queue.empty(); });
            if (m_stopFlag.load()) break;
            if (!m_queue.empty()) {
                msg = m_queue.front();
                m_queue.pop_front();
                hasMsg = true;
            }
        }

        /* 取一次时间戳，本轮 tick 共用（避免多次 nowMs() 导致时间不一致） */
        const int64_t now = nowMs();

        /* 信令连接维护：不持 m_mutex（connect 阻塞可达 10s，
         * 期间 IPC 线程的 status()/手动推流不受影响） */
        if (m_signaling && !m_signaling->isConnected()) {
            if (now - lastConnAttempt >= kSigRetryMs) {
                lastConnAttempt = now;
                m_signaling->connect();
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (hasMsg) processMessage(msg);
        tickLocked(now);
    }
}

void LiveStreamService::processMessage(const SigMsg& msg) {
    if (msg.sid.empty()) return;

    if (msg.type == "offer") {
        if (m_paused.load()) {
            /* 相机重启窗口：拒绝新会话，观看端收 bye 后稍后重试 */
            m_signaling->sendBye(msg.sid, "camera_restarting");
            return;
        }
        /* offer 可携带 mode 字段，指定本次会话的推流模式（不传默认 P2P） */
        if (msg.mode == "p2p_host") {
            m_pushMode = PushMode::P2P_Host;
        } else {
            m_pushMode = PushMode::P2P;
        }
        NC_LOGI("LiveStreamService: offer sid={} (state={} push={})",
                msg.sid.c_str(), stateName(m_mode), pushModeName(m_pushMode));
    
        /* 前端只发 P2P/P2P_Host offer，SFU 走 RPC 不走信令 */
        handleOfferP2P(msg.sid, msg.body);
        return;
    }

    if (msg.type == "candidate") {
        if (m_mode == Mode::P2P && msg.sid == m_p2pSid) {
            m_session.handleRemoteCandidate(msg.body);
        }
        return;
    }

    if (msg.type == "bye") {
        if (m_mode == Mode::P2P && msg.sid == m_p2pSid) {
            NC_LOGI("LiveStreamService: viewer bye, p2p -> idle");
            teardownAllLocked(false);
        } else if (m_mode == Mode::SFU) {
            /* 校验 sid：只有已登记的 SFU 观众才允许减计数
             * 防止过期 bye（如旧 P2P 会话的 bye）误杀活跃推流 */
            auto erased = m_sfuViewers.erase(msg.sid);
            if (erased > 0) {
                NC_LOGI("LiveStreamService: sfu viewer left sid={} (remain={})",
                        msg.sid.c_str(), m_sfuViewers.size());
                if (m_sfuViewers.empty()) {
                    /* 所有观看者都走了，清除手动推流标志，回 idle */
                    m_manualPush = false;
                    NC_LOGI("LiveStreamService: no sfu viewers, stop push -> idle");
                    teardownAllLocked(false);
                }
            } else {
                NC_LOGD("LiveStreamService: sfu bye for unknown sid={}, ignoring",
                        msg.sid.c_str());
            }
        }
        return;
    }

    if (msg.type == "alive") {
        if (m_mode == Mode::SFU) {
            /* 未登记的 sid 也接纳（进程重启后残留观看者的自愈路径） */
            m_sfuViewers[msg.sid] = nowMs();
        }
        return;
    }
}

void LiveStreamService::tickLocked(int64_t now) {
    if (m_mode == Mode::P2P) {
        if (!m_p2pConnected) {
            if (m_session.isConnected()) {
                /* ICE/DTLS/SRTP 就绪：启动发送管线并请求 IDR（避免花屏） */
                m_p2pConnected = true;
                m_session.startSending();
                requestKeyFrame();
                NC_LOGI("LiveStreamService: p2p connected sid={} (%.1fs)",
                        m_p2pSid.c_str(), (now - m_p2pStartMs) / 1000.0);
            } else if (now - m_p2pStartMs > (int64_t)m_cfg.p2pTimeoutSec * 1000) {
                /* P2P 超时：发 bye 通知前端，前端自行决定是否走 SFU */
                NC_LOGW("LiveStreamService: p2p timeout, notifying frontend");
                std::string sid = m_p2pSid;
                teardownAllLocked(false);
                m_signaling->sendBye(sid, "timeout");
            }
        } else if (m_session.isPeerClosed()) {
            /* DTLS close_notify：信令 bye 丢失时的兑底终止信号 */
            NC_LOGI("LiveStreamService: peer closed (dtls), p2p -> idle");
            teardownAllLocked(false);
        }
        return;
    }

    if (m_mode == Mode::SFU) {
        /* 心跳清理：观看端 10s 一跳，relayIdleSec(>=15) 无心跳视为离开 */
        for (auto it = m_sfuViewers.begin(); it != m_sfuViewers.end();) {
            if (now - it->second > (int64_t)m_cfg.relayIdleSec * 1000) {
                NC_LOGI("LiveStreamService: sfu viewer timeout sid={}",
                        it->first.c_str());
                it = m_sfuViewers.erase(it);
            } else {
                ++it;
            }
        }
        if (m_sfuViewers.empty() && !m_manualPush) {
            /* 所有观看者心跳都超时了，且不是手动推流，回 idle */
            NC_LOGI("LiveStreamService: all sfu viewers gone, stop push -> idle");
            teardownAllLocked(false);
        }
    }
}

/* ==================== 状态机动作（持 m_mutex） ==================== */

/* P2P 模式：严格 P2P，失败/多人发 bye 通知前端，不做自动降级
 * 前端收到通知后自行决定是否 RPC startLiveStream 走 SFU */
void LiveStreamService::handleOfferP2P(const std::string& sid, const std::string& offerSdp) {
    switch (m_mode) {
    case Mode::Idle:
        if (!enterP2P(sid, offerSdp)) {
            /* P2P 失败，发 bye 通知前端 */
            NC_LOGW("LiveStreamService: p2p offer failed, notifying frontend");
            m_signaling->sendBye(sid, "negotiate_failed");
        }
        break;
    case Mode::P2P:
        if (sid == m_p2pSid) {
            /* 同一观看者重新协商（页面刷新）：重建 P2P 会话 */
            m_session.stopSession();
            m_p2pConnected = false;
            if (!enterP2P(sid, offerSdp)) {
                NC_LOGW("LiveStreamService: p2p renegotiate failed, notifying frontend");
                teardownAllLocked(false);
                m_signaling->sendBye(sid, "negotiate_failed");
            }
        } else {
            /* P2P 只能 1 人看，第 2 人直接拒绝 */
            NC_LOGI("LiveStreamService: p2p rejected, already connected (sid={})", sid.c_str());
            m_signaling->sendBye(sid, "p2p_occupied");
        }
        break;
    case Mode::SFU:
        /* SFU 模式下有人请求 P2P：直接拒绝，告诉前端当前是多人模式 */
        NC_LOGI("LiveStreamService: p2p rejected, sfu active (viewers={})", m_sfuViewers.size());
        m_signaling->sendBye(sid, "sfu_active");
        break;
    }
}

bool LiveStreamService::enterP2P(const std::string& sid, const std::string& offerSdp) {
    camera::CameraDevice* dev = m_camera.device();
    if (dev == nullptr) {
        NC_LOGW("LiveStreamService: no camera device, reject offer");
        m_signaling->sendBye(sid, "no_camera");
        return true;   /* 已答复观看端（发了 bye），调用方不需要再做任何事 */
    }

    /* 候选回调可能在 handleOffer 内即触发（gather 线程），先登记 sid */
    {
        std::lock_guard<std::mutex> lock(m_sidMutex);
        m_p2pSid = sid;
    }

    /* 视频参数取相机实际生效值（applyConfig 失败时与 ConfigStore 可能不一致） */
    const camera::ChannelConfig& ch = dev->config().mainChannel();
    rtc::P2PSessionConfig cfg;
    cfg.width   = ch.width;
    cfg.height  = ch.height;
    cfg.fps     = ch.fps;
    cfg.bitrate = ch.bitrate;

    /* P2P_Host 模式：清空 ICE 配置，只收集 host 候选（局域网直连）
     * P2P 模式：用配置的 TURN 服务器（跨网络 NAT 穿透） */
    if (m_pushMode == PushMode::P2P_Host) {
        cfg.ice.host.clear();     /* host 为空 → candidateType=0 → 仅 Host 候选 */
        cfg.ice.username.clear();
        cfg.ice.password.clear();
    } else {
        cfg.ice.host     = m_cfg.ice.host;
        cfg.ice.port     = m_cfg.ice.port;
        cfg.ice.username = m_cfg.ice.username;
        cfg.ice.password = m_cfg.ice.password;
    }

    std::string answer;
    if (!m_session.handleOffer(cfg, offerSdp, answer)) {
        std::lock_guard<std::mutex> lock(m_sidMutex);
        m_p2pSid.clear();
        return false;
    }
    if (!m_signaling->sendAnswer(sid, answer)) {
        NC_LOGE("LiveStreamService: send answer failed");
        m_session.stopSession();
        std::lock_guard<std::mutex> lock(m_sidMutex);
        m_p2pSid.clear();
        return false;
    }

    subscribeMain();
    m_mode = Mode::P2P;
    m_p2pConnected = false;
    m_p2pStartMs = nowMs();
    NC_LOGI("LiveStreamService: p2p session created sid={} ({}x{}@{} {}kbps ice={})",
            sid.c_str(), cfg.width, cfg.height, cfg.fps, cfg.bitrate,
            cfg.ice.host.empty() ? "<host-only>" : cfg.ice.host.c_str());
    return true;
}

bool LiveStreamService::ensureWhipPushing() {
    /* 已在推且 token 一致：幂等返回（同模式重复请求不重启） */
    if (m_pusher.isPushing()) {
        if (m_accessToken == m_pushToken) return true;
        /* token 变了：停旧推，用新 token 重建（保留主通道订阅）
         * 注意：WHIP 重启需要 500ms 等待（OS socket 释放 + SRS 清理旧会话），
         * 期间持 m_mutex——可接受：startManualPush 是低频 RPC，不影响其他功能 */
        NC_LOGI("LiveStreamService: token changed, restarting whip push");
        m_pusher.stopPush();
        usleep(500 * 1000);
        /* 重新检查相机是否还在（500ms 等待期间 stop() 可能已清理设备） */
        camera::CameraDevice* devAfter = m_camera.device();
        if (!devAfter) {
            NC_LOGW("LiveStreamService: camera gone after whip restart delay");
            return false;
        }
        m_pushToken = m_accessToken;
        const camera::ChannelConfig& ch = devAfter->config().mainChannel();
        rtc::WhipConfig cfg;
        cfg.width = ch.width; cfg.height = ch.height;
        cfg.fps = ch.fps; cfg.bitrate = ch.bitrate;
        const std::string whipUrl = m_cfg.whipUrl(m_devId, m_pushToken);
        if (!m_pusher.startPush(cfg, whipUrl)) {
            NC_LOGE("LiveStreamService: whip restart failed url={}", whipUrl.c_str());
            return false;
        }
        requestKeyFrame();
        return true;
    }

    camera::CameraDevice* dev = m_camera.device();
    if (dev == nullptr) {
        NC_LOGW("LiveStreamService: no camera device, cannot push");
        return false;
    }

    const camera::ChannelConfig& ch = dev->config().mainChannel();
    rtc::WhipConfig cfg;
    cfg.width   = ch.width;
    cfg.height  = ch.height;
    cfg.fps     = ch.fps;
    cfg.bitrate = ch.bitrate;

    /* 阻塞式：HTTP SDP 交换 + 等待就绪（最长 10s），期间持 m_mutex——
     * 可接受：仅发生在切 SFU 的瞬间，且 SRS 公网直连通常 <1s */
    m_pushToken = m_accessToken;
    const std::string whipUrl = m_cfg.whipUrl(m_devId, m_pushToken);
    if (!m_pusher.startPush(cfg, whipUrl)) {
        NC_LOGE("LiveStreamService: whip push failed url={}", whipUrl.c_str());
        return false;
    }
    subscribeMain();
    requestKeyFrame();
    return true;
}

void LiveStreamService::teardownAllLocked(bool notifyPeers) {
    if (m_mode == Mode::P2P && notifyPeers && m_signaling && !m_p2pSid.empty()) {
        m_signaling->sendBye(m_p2pSid);
    }
    if (notifyPeers && m_signaling) {
        for (const auto& kv : m_sfuViewers) {
            m_signaling->sendBye(kv.first);
        }
    }
    m_session.stopSession();
    m_pusher.stopPush();
    m_pushToken.clear();
    m_sfuViewers.clear();
    m_p2pConnected = false;
    m_manualPush = false;
    {
        std::lock_guard<std::mutex> lock(m_sidMutex);
        m_p2pSid.clear();
    }
    unsubscribeMain();
    m_mode = Mode::Idle;
    m_pushMode = PushMode::P2P;  /* 会话结束，模式偏好重置为默认 P2P */
}

/* ==================== 帧路由 ==================== */

void LiveStreamService::subscribeMain() {
    if (m_subId != camera::kInvalidSubscriptionId) return;
    camera::CameraDevice* dev = m_camera.device();
    if (dev == nullptr) return;

    /* 采集线程回调：直通两个发送管线，各自内部按 running 标志丢弃，
     * 无需加锁（帧数据在 pushFrame 内拷贝入队，回调返回后即失效） */
    m_subId = dev->subscribe(camera::ChannelId::Main,
        [this](const camera::VideoFrame& frame) {
            m_session.pushFrame(frame.data(), (int)frame.size(),
                                (uint64_t)frame.timestamp(), frame.isKeyFrame());
            m_pusher.pushFrame(frame.data(), (int)frame.size(),
                               (uint64_t)frame.timestamp(), frame.isKeyFrame());
        });
    NC_LOGI("LiveStreamService: subscribed main channel (subId={})", m_subId);
}

void LiveStreamService::unsubscribeMain() {
    if (m_subId == camera::kInvalidSubscriptionId) return;
    camera::CameraDevice* dev = m_camera.device();
    if (dev) dev->unsubscribe(m_subId);
    m_subId = camera::kInvalidSubscriptionId;
    NC_LOGI("LiveStreamService: unsubscribed main channel");
}

void LiveStreamService::requestKeyFrame() {
    camera::CameraDevice* dev = m_camera.device();
    if (dev) dev->requestKeyFrame(camera::ChannelId::Main);
}

} // namespace mediad
