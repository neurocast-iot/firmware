/**
 * @file CameraService.cpp
 * @brief 相机服务实现
 */
#include "services/CameraService.h"

#include "nc/common/log_utils.h"

#include <algorithm>
#include <chrono>

namespace mediad {

CameraService::~CameraService() {
    stop();
}

bool CameraService::init(const CameraCfg& cfg) {
    try {
        m_device = std::make_unique<camera::CameraDevice>(toCameraConfig(cfg));
    } catch (const std::exception& e) {
        NC_LOGE("CameraService: invalid camera config: {}", e.what());
        return false;
    }
    /* camera 库日志级别：0=ERROR, 1=WARN, 2=INFO, 3=DEBUG */
    m_device->setLogCallback([](int level, const std::string& msg) {
        switch (level) {
            case 0:  NC_LOGE("[camera] {}", msg.c_str()); break;
            case 1:  NC_LOGW("[camera] {}", msg.c_str()); break;
            case 3:  NC_LOGD("[camera] {}", msg.c_str()); break;
            default: NC_LOGI("[camera] {}", msg.c_str()); break;
        }
    });
    return true;
}

void CameraService::setOnStarted(Hook cb) {
    m_onStarted = std::move(cb);
}

void CameraService::setRestartHooks(Hook willRestart, Hook restartDone) {
    m_willRestart = std::move(willRestart);
    m_restartDone = std::move(restartDone);
}

void CameraService::startAsync() {
    if (!m_device) {
        NC_LOGE("CameraService: device not initialized, startAsync ignored");
        return;
    }
    if (m_thread.joinable()) {
        return;   /* 已在启动流程中 */
    }
    m_stopFlag = false;
    m_thread = std::thread(&CameraService::retryLoop, this);
}

void CameraService::retryLoop() {
    int delaySec = 1;
    while (!m_stopFlag) {
        camera::Error err = m_device->start();
        if (err == camera::Error::Ok) {
            NC_LOGI("CameraService: camera started");
            if (m_onStarted) {
                m_onStarted();
            }
            return;
        }
        NC_LOGW("CameraService: start failed (err={}), retry in {}s",
                static_cast<int>(err), delaySec);
        /* 可打断睡眠：stop() 时立即退出 */
        std::unique_lock<std::mutex> lock(m_cvMutex);
        m_cv.wait_for(lock, std::chrono::seconds(delaySec),
                      [this] { return m_stopFlag.load(); });
        delaySec = std::min(delaySec * 2, 30);
    }
}

void CameraService::stop() {
    m_stopFlag = true;
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_device) {
        m_device->stop();
    }
}

bool CameraService::applyCameraConfig(const CameraCfg& cfg, bool& restarted,
                                      std::string& errMsg) {
    restarted = false;
    if (!m_device) {
        errMsg = "camera device not initialized";
        return false;
    }
    if (cfg.sensorConfig != m_device->config().sensorConfig()) {
        NC_LOGW("CameraService: sensor_config changed, requires process restart to take effect");
    }

    camera::CameraConfig newConfig;
    try {
        newConfig = toCameraConfig(cfg);
    } catch (const std::exception& e) {
        errMsg = e.what();
        return false;
    }

    const bool willRestart = m_device->checkWillRestart(newConfig);
    if (willRestart && m_willRestart) {
        m_willRestart();   /* 分辨率将变：录像收卷等前置清场 */
    }

    camera::ApplyResult result = m_device->applyConfig(newConfig);
    restarted = result.restarted;

    if (willRestart && m_restartDone) {
        m_restartDone();   /* 通知重启结束，回调内部判断成败决定是否恢复 */
    }

    if (!result.success) {
        errMsg = result.errorMsg;
        NC_LOGE("CameraService: applyConfig failed: {}", errMsg.c_str());
        return false;
    }
    NC_LOGI("CameraService: config applied (restarted={})", restarted ? 1 : 0);
    return true;
}

bool CameraService::isRunning() const {
    return m_device && m_device->isRunning();
}

} // namespace mediad
