/**
 * @file camera_device_impl.cpp
 * @brief CameraDevice Pimpl 实现 — 编排层（平台无关的业务逻辑）
 *
 * CameraDevice::Impl 是 CameraDevice 对外接口的唯一后端，负责：
 *   - 拍照编排：取帧 → 驱动编码 JPEG → 写文件
 *   - 配置应用编排：diff 分辨率 → restart 或热更新 fps/bitrate
 *   - 订阅管理：subscriber 0→1 时 startStream，1→0 时 stopStream
 *
 * 所有 SDK 依赖均限定在平台驱动实现内部，本文件不包含任何厂商头文件；
 * 驱动实例通过 driver_factory 按编译期平台创建，新增平台本文件零改动。
 */
#include "camera/camera_device.h"
#include "camera/camera_driver.h"
#include "camera/frame.h"
#include "camera/error.h"

/* 平台驱动工厂（内部头，按编译期链接对应平台的实现） */
#include "driver_factory.h"

#include <mutex>
#include <atomic>
#include <cstdio>
#include <vector>
#include <chrono>
#include <thread>

namespace camera {

/* ================================================================
 * CameraDevice::Impl 定义（私有实现类 — 编排层）
 * ================================================================ */

class CameraDevice::Impl {
public:
    explicit Impl(const CameraConfig& config)
        : m_config(config)
        , m_state(DeviceState::Idle)
    {
        /* 工厂按编译期平台创建驱动（含各平台自己的 SDK 全局初始化） */
        m_driver = createPlatformDriver(config);
        /* 绑定事件分发器 */
        m_driver->setDispatcher(&m_dispatcher);
    }

    ~Impl() {
        stop();
    }

    /* ------------------------------------------------
     * 生命周期管理
     * ------------------------------------------------ */

    Error start() {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state == DeviceState::Running) {
            return Error::Ok;
        }
        if (m_state == DeviceState::Error) {
            return Error::InvalidState;
        }

        m_state = DeviceState::Configured;

        if (m_logCb) {
            m_driver->setLogCallback(m_logCb);
        }

        Error err = m_driver->init();
        if (err != Error::Ok) {
            m_state = DeviceState::Error;
            return err;
        }

        m_state = DeviceState::Running;
        log(2, "[Camera] started");
        return Error::Ok;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state == DeviceState::Idle) {
            return;
        }

        m_state = DeviceState::Stopping;
        m_driver->cleanup();
        m_state = DeviceState::Idle;
        log(2, "[Camera] stopped");
    }

    /* ------------------------------------------------
     * 订阅管理（观察者模式）
     *
     * subscriber 0→1：startStream（驱动内部 enableVI + 编码器 + 采集线程）
     * subscriber 1→0：stopStream（驱动内部停止采集 + disableVI）
     * ------------------------------------------------ */

    SubscriptionId subscribe(ChannelId channel, FrameCallback callback) {
        std::lock_guard<std::mutex> lock(m_mutex);

        SubscriptionId id = m_dispatcher.subscribe(channel, std::move(callback));
        if (id == kInvalidSubscriptionId) {
            return id;
        }

        if (m_state != DeviceState::Running) {
            return id;
        }

        /* 订阅数从 0→1：启动推流 */
        if (m_dispatcher.subscriberCount(channel) == 1) {
            log(2, "[Camera] first subscriber for channel "
                   + std::to_string(static_cast<int>(channel))
                   + ", starting stream...");
            Error err = m_driver->startStream(channel);
            if (err != Error::Ok) {
                log(0, "[Camera] failed to start stream for channel "
                       + std::to_string(static_cast<int>(channel)));
                m_dispatcher.unsubscribe(id);
                return kInvalidSubscriptionId;
            }
            log(2, "[Camera] channel "
                   + std::to_string(static_cast<int>(channel)) + " streaming");
        }

        return id;
    }

    void unsubscribe(SubscriptionId id) {
        std::lock_guard<std::mutex> lock(m_mutex);

        ChannelId channel = m_dispatcher.getChannelId(id);
        size_t countBefore = m_dispatcher.subscriberCount(channel);

        m_dispatcher.unsubscribe(id);

        if (m_state != DeviceState::Running) {
            return;
        }

        /* 订阅数从 1→0：停止推流 */
        if (countBefore == 1 && m_dispatcher.subscriberCount(channel) == 0) {
            log(2, "[Camera] last subscriber removed for channel "
                   + std::to_string(static_cast<int>(channel))
                   + ", stopping stream...");
            m_driver->stopStream(channel);
            log(2, "[Camera] channel "
                   + std::to_string(static_cast<int>(channel)) + " stopped");
        }
    }

    /* ------------------------------------------------
     * 拍照能力启停（按需，和推流订阅一样）
     * ------------------------------------------------ */

    Error startSnapshot() {
        if (m_state != DeviceState::Running || !m_driver) {
            return Error::InvalidState;
        }
        return m_driver->startSnapshot();
    }

    void stopSnapshot() {
        if (m_state == DeviceState::Running && m_driver) {
            m_driver->stopSnapshot();
        }
    }

    /* ------------------------------------------------
     * 缩略图能力启停（和拍照同生命周期）
     * ------------------------------------------------ */

    Error startThumbnail(uint16_t width, uint16_t height) {
        if (m_state != DeviceState::Running || !m_driver) {
            return Error::InvalidState;
        }
        return m_driver->startThumbnail(width, height);
    }

    void stopThumbnail() {
        if (m_state == DeviceState::Running && m_driver) {
            m_driver->stopThumbnail();
        }
    }

    Error captureThumbnail(std::vector<uint8_t>& thumbData) {
        if (m_state != DeviceState::Running) return Error::InvalidState;

        Error err = m_driver->captureThumbnail(thumbData);
        if (err != Error::Ok) {
            log(0, "[Camera] captureThumbnail failed: " + std::to_string((int)err));
        }
        return err;
    }

    /* ------------------------------------------------
     * 移动侦测（转发给驱动）
     * ------------------------------------------------ */

    Error startMotionDetection(const MotionDetectConfig& cfg, MotionCallback onMotion) {
        if (m_state != DeviceState::Running || !m_driver) {
            return Error::InvalidState;
        }
        return m_driver->startMotionDetection(cfg, std::move(onMotion));
    }

    void stopMotionDetection() {
        if (m_state == DeviceState::Running && m_driver) {
            m_driver->stopMotionDetection();
        }
    }

    uint64_t getThumbFrameSeq() const {
        if (m_state != DeviceState::Running || !m_driver) return 0;
        return m_driver->getThumbFrameSeq();
    }

    /* ------------------------------------------------
     * 配置应用编排（平台无关）
     *
     * checkWillRestart: 比较分辨率是否变化
     * applyConfig:
     *   分辨率变化 → driver.updateConfig + driver.restart + 恢复推流
     *   仅 fps/kbps → driver.updateConfig + 热更新 setFps/setBitrate
     * ------------------------------------------------ */

    bool checkWillRestart(const CameraConfig& newConfig) const {
        if (m_state != DeviceState::Running) return false;

        /* 主通道分辨率变化 → 需要重启 */
        if (newConfig.mainChannel().width > 0 &&
            (newConfig.mainChannel().width  != m_config.mainChannel().width ||
             newConfig.mainChannel().height != m_config.mainChannel().height)) {
            return true;
        }

        /* 子通道分辨率变化 → 需要重启（Anyka SDK 约束） */
        if (newConfig.subEnabled() &&
            newConfig.subChannel().width > 0 &&
            (newConfig.subChannel().width  != m_config.subChannel().width ||
             newConfig.subChannel().height != m_config.subChannel().height)) {
            return true;
        }

        return false;
    }

    ApplyResult applyConfig(const CameraConfig& newConfig) {
        if (m_state != DeviceState::Running) {
            return {false, false, Error::InvalidState, "Device not running"};
        }

        bool needRestart = checkWillRestart(newConfig);

        /* 更新驱动内部配置副本 */
        m_driver->updateConfig(newConfig);

        if (needRestart) {
            /* 分辨率变化 → 必须重启设备 */
            log(2, "[Camera] resolution changed, restarting device...");
            Error err = m_driver->restart();
            if (err != Error::Ok) {
                return {false, true, err, "Restart failed after config change"};
            }
            /* restart() 内部自动恢复之前推流中的通道 */
            m_config = newConfig;
            log(2, "[Camera] device restarted with new config");
            return {true, true, Error::Ok, ""};
        }

        /* 仅 fps/bitrate 变化 → 热更新（不需要重启） */
        std::string changes;
        if (newConfig.mainChannel().width > 0) {
            if (newConfig.mainChannel().fps != m_config.mainChannel().fps) {
                Error e = m_driver->setFps(ChannelId::Main, newConfig.mainChannel().fps);
                changes += " main:fps=" + std::to_string(newConfig.mainChannel().fps);
                if (e != Error::Ok) {
                    changes += "(FAIL)";
                }
            }
            if (newConfig.mainChannel().bitrate != m_config.mainChannel().bitrate) {
                Error e = m_driver->setBitrate(ChannelId::Main, newConfig.mainChannel().bitrate);
                changes += " main:bitrate=" + std::to_string(newConfig.mainChannel().bitrate) + "kbps";
                if (e != Error::Ok) {
                    changes += "(FAIL)";
                }
            }
        }
        if (newConfig.subEnabled() && newConfig.subChannel().width > 0) {
            if (newConfig.subChannel().fps != m_config.subChannel().fps) {
                Error e = m_driver->setFps(ChannelId::Sub, newConfig.subChannel().fps);
                changes += " sub:fps=" + std::to_string(newConfig.subChannel().fps);
                if (e != Error::Ok) {
                    changes += "(FAIL)";
                }
            }
            if (newConfig.subChannel().bitrate != m_config.subChannel().bitrate) {
                Error e = m_driver->setBitrate(ChannelId::Sub, newConfig.subChannel().bitrate);
                changes += " sub:bitrate=" + std::to_string(newConfig.subChannel().bitrate) + "kbps";
                if (e != Error::Ok) {
                    changes += "(FAIL)";
                }
            }
        }

        m_config = newConfig;
        log(2, "[Camera] hot-swapped:" + (changes.empty() ? std::string(" (no change)") : changes));
        return {true, false, Error::Ok, ""};
    }

    /* ------------------------------------------------
     * 运行时控制
     * ------------------------------------------------ */

    Error requestKeyFrame(ChannelId channel) {
        if (m_state != DeviceState::Running) return Error::InvalidState;
        return m_driver->requestKeyFrame(channel);
    }

    /**
     * 编码器休眠/唤醒
     *
     * 在新架构中，编码器生命周期由 startStream/stopStream 管理。
     * 此接口保留以兼容上层 API，内部通过 startStream/stopStream 实现。
     */
    Error setEncoderActive(ChannelId channel, bool active) {
        if (m_state != DeviceState::Running) return Error::InvalidState;

        if (active && m_driver->getChannelState(channel) != ChannelState::Streaming) {
            return m_driver->startStream(channel);
        }
        if (!active && m_driver->getChannelState(channel) == ChannelState::Streaming) {
            m_driver->stopStream(channel);
        }
        return Error::Ok;
    }

    bool isEncoderActive(ChannelId channel) const {
        return m_driver->getChannelState(channel) == ChannelState::Streaming;
    }

    /* ------------------------------------------------
     * 拍照编排（平台无关）
     *
     * 优先走常驻 JPEG 硬件编码（captureJpeg）：
     *   驱动内部有后台线程持续取编码好的 JPEG 帧，
     *   这里直接读 buffer，不需要等视频输入出帧，不会 timeout。
     *
     * 如果驱动不支持（返回 NotImplemented），回退到旧路：
     *   acquireFrame 取 YUV 帧 → 驱动按需编码 JPEG → 写文件。
     *   编码动作同样走驱动接口，编排层不碰任何平台编码器。
     * ------------------------------------------------ */

    Error captureSnapshot(ChannelId channel,
                          const std::string& outputPath,
                          int quality) {
        if (m_state != DeviceState::Running) return Error::InvalidState;

        std::vector<uint8_t> jpegData;

        /* 优先走常驻 JPEG 硬件编码（不需要等视频输入出帧） */
        Error err = m_driver->captureJpeg(channel, jpegData);

        if (err == Error::NotImplemented) {
            /* 驱动不支持常驻编码，回退到旧路：取 YUV 帧 + 按需编码 */
            RawFrame frame;
            const int maxRetries = 5;
            for (int retry = 0; retry < maxRetries; ++retry) {
                err = m_driver->acquireFrame(channel, frame);
                if (err == Error::Ok) break;
                if (retry < maxRetries - 1)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (err != Error::Ok) {
                log(0, "[Camera] captureSnapshot: acquireFrame failed after "
                       + std::to_string(maxRetries) + " retries");
                return err;
            }

            err = m_driver->encodeRawToJpeg(channel, frame.data, frame.len,
                                            quality, jpegData);
            m_driver->releaseFrame(channel, frame);
            if (err != Error::Ok) {
                log(0, "[Camera] captureSnapshot: JPEG encode failed");
                return err;
            }
        } else if (err != Error::Ok) {
            log(0, "[Camera] captureSnapshot: captureJpeg failed: " + std::to_string((int)err));
            return err;
        }

        /* 写入文件 */
        FILE* fp = fopen(outputPath.c_str(), "wb");
        if (!fp) {
            log(0, "[Camera] captureSnapshot: fopen failed: " + outputPath);
            return Error::Unknown;
        }
        size_t written = fwrite(jpegData.data(), 1, jpegData.size(), fp);
        fclose(fp);

        if (written != jpegData.size()) {
            log(0, "[Camera] captureSnapshot: fwrite incomplete");
            return Error::Unknown;
        }

        log(2, "[Camera] snapshot saved: " + outputPath
               + " (" + std::to_string(jpegData.size()) + " bytes)");
        return Error::Ok;
    }

    /* ------------------------------------------------
     * ISP 控制
     * ------------------------------------------------ */

    Error setDayNightMode(DayNightMode mode) {
        if (m_state != DeviceState::Running) return Error::InvalidState;
        return m_driver->setDayNightMode(mode);
    }

    /* ------------------------------------------------
     * 状态查询
     * ------------------------------------------------ */

    DeviceState state() const noexcept {
        return m_state.load();
    }

    bool isRunning() const noexcept {
        return m_state.load() == DeviceState::Running;
    }

    const CameraConfig& config() const noexcept {
        return m_config;
    }

    Error getHardwareCapability(int& mainMaxW, int& mainMaxH,
                                int& subMaxW,  int& subMaxH) const {
        DriverCapabilities caps = m_driver->getCapabilities();
        mainMaxW = caps.mainMaxWidth;
        mainMaxH = caps.mainMaxHeight;
        subMaxW  = caps.subMaxWidth;
        subMaxH  = caps.subMaxHeight;
        return Error::Ok;
    }

    /* ------------------------------------------------
     * 日志系统
     * ------------------------------------------------ */

    void setLogCallback(CameraDevice::LogCallback cb) {
        m_logCb = std::move(cb);
        m_driver->setLogCallback(m_logCb);
    }

private:
    void log(int level, const std::string& msg) {
        if (m_logCb) {
            m_logCb(level, msg);
        } else {
            FILE* out = (level <= 1) ? stderr : stdout;
            fprintf(out, "%s\n", msg.c_str());
        }
    }

    /* 成员变量 */
    CameraConfig                  m_config;
    std::atomic<DeviceState>      m_state;
    mutable std::mutex            m_mutex;

    EventDispatcher               m_dispatcher;
    std::unique_ptr<CameraDriver> m_driver;  /* 驱动抽象层（可替换） */

    CameraDevice::LogCallback     m_logCb;
};

/* ================================================================
 * CameraDevice 公共接口实现（全部委托给 Impl）
 * ================================================================ */

CameraDevice::CameraDevice(const CameraConfig& config)
    : m_impl(new Impl(config)) {
    /* 各平台的 SDK 全局初始化已收进驱动工厂，这里不做平台相关动作 */
}

CameraDevice::~CameraDevice() {
    /* 不做 SDK 全局清理：SDK 是进程级资源，单个相机实例析构关掉它，
     * 之后创建的实例全部失效（mediad 重启相机会踩到这个）。
     * 进程退出由系统回收 SDK 资源。 */
}

Error CameraDevice::start() {
    return m_impl->start();
}

void CameraDevice::stop() {
    m_impl->stop();
}

SubscriptionId CameraDevice::subscribe(ChannelId channel, FrameCallback callback) {
    return m_impl->subscribe(channel, std::move(callback));
}

void CameraDevice::unsubscribe(SubscriptionId id) {
    m_impl->unsubscribe(id);
}

Error CameraDevice::startSnapshot() {
    return m_impl->startSnapshot();
}

void CameraDevice::stopSnapshot() {
    m_impl->stopSnapshot();
}

bool CameraDevice::checkWillRestart(const CameraConfig& newConfig) const {
    return m_impl->checkWillRestart(newConfig);
}

ApplyResult CameraDevice::applyConfig(const CameraConfig& newConfig) {
    return m_impl->applyConfig(newConfig);
}

Error CameraDevice::requestKeyFrame(ChannelId channel) {
    return m_impl->requestKeyFrame(channel);
}

Error CameraDevice::setEncoderActive(ChannelId channel, bool active) {
    return m_impl->setEncoderActive(channel, active);
}

bool CameraDevice::isEncoderActive(ChannelId channel) const {
    return m_impl->isEncoderActive(channel);
}

Error CameraDevice::captureSnapshot(ChannelId channel,
                                    const std::string& outputPath,
                                    int quality) {
    return m_impl->captureSnapshot(channel, outputPath, quality);
}

Error CameraDevice::captureThumbnail(std::vector<uint8_t>& thumbData) {
    return m_impl->captureThumbnail(thumbData);
}

Error CameraDevice::startThumbnail(uint16_t width, uint16_t height) {
    return m_impl->startThumbnail(width, height);
}

void CameraDevice::stopThumbnail() {
    m_impl->stopThumbnail();
}

Error CameraDevice::startMotionDetection(const MotionDetectConfig& cfg, MotionCallback onMotion) {
    return m_impl->startMotionDetection(cfg, std::move(onMotion));
}

void CameraDevice::stopMotionDetection() {
    m_impl->stopMotionDetection();
}

uint64_t CameraDevice::getThumbFrameSeq() const {
    return m_impl->getThumbFrameSeq();
}

Error CameraDevice::setDayNightMode(DayNightMode mode) {
    return m_impl->setDayNightMode(mode);
}

DeviceState CameraDevice::state() const noexcept {
    return m_impl->state();
}

bool CameraDevice::isRunning() const noexcept {
    return m_impl->isRunning();
}

const CameraConfig& CameraDevice::config() const noexcept {
    return m_impl->config();
}

Error CameraDevice::getHardwareCapability(int& mainMaxW, int& mainMaxH,
                                          int& subMaxW,  int& subMaxH) const {
    return m_impl->getHardwareCapability(mainMaxW, mainMaxH, subMaxW, subMaxH);
}

void CameraDevice::setLogCallback(LogCallback callback) {
    m_impl->setLogCallback(std::move(callback));
}

} // namespace camera
