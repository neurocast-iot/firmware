/**
 * @file CameraService.h
 * @brief 相机服务：CameraDevice 的生命周期与配置编排
 *
 * 职责：
 *   - 持有唯一的 CameraDevice 实例（进程内相机所有权归此服务）
 *   - autoStart 启动：后台退避重试线程（1s→2s→…→30s 封顶），
 *     启动失败不退出进程（服务永不退出原则），成功后触发 onStarted 钩子
 *   - 配置热更编排：checkWillRestart → willRestart 钩子（录像收卷等）
 *     → applyConfig → restartDone 钩子（OSD 重绑分辨率、恢复录像）
 *
 * 钩子约定：restartDone 钩子内应从 device()->config() 读取实际生效的
 * 分辨率（而非 ConfigStore），保证 applyConfig 失败时状态仍然一致。
 */
#pragma once

#include "config/ConfigStore.h"

#include "camera/camera_device.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mediad {

class CameraService {
public:
    using Hook = std::function<void()>;

    CameraService() = default;
    ~CameraService();

    CameraService(const CameraService&) = delete;
    CameraService& operator=(const CameraService&) = delete;

    /**
     * 创建 CameraDevice（不碰硬件，仅校验并保存配置）
     * @return false=配置非法（device 保持为空，后续操作自动降级）
     */
    bool init(const CameraCfg& cfg);

    /** 相机启动成功回调（在重试线程中触发，仅首次 start 成功时） */
    void setOnStarted(Hook cb);

    /**
     * 注册重启编排钩子
     * @param willRestart 分辨率变更前调用（如录像收卷）
     * @param restartDone applyConfig 编排结束后调用（回调内部判断成败决定是否恢复）
     */
    void setRestartHooks(Hook willRestart, Hook restartDone);

    /** 启动相机（后台退避重试直到成功或 stop） */
    void startAsync();

    /** 停止重试线程并关闭相机（幂等） */
    void stop();

    /**
     * 应用相机配置（IPC 配置热更入口）
     *
     * 仅处理通道参数（分辨率触发重启编排，fps/码率热更）；
     * sensorConfig 变更需重启进程才生效（记录日志提示）。
     *
     * @param restarted 输出：是否发生了编码器重启
     */
    bool applyCameraConfig(const CameraCfg& cfg, bool& restarted, std::string& errMsg);

    /** 底层设备（init 失败时为 nullptr，调用方须判空） */
    camera::CameraDevice* device() { return m_device.get(); }

    bool isRunning() const;

private:
    void retryLoop();

    std::unique_ptr<camera::CameraDevice> m_device;

    Hook m_onStarted;
    Hook m_willRestart;
    Hook m_restartDone;

    std::thread             m_thread;
    std::atomic<bool>       m_stopFlag{false};
    std::mutex              m_cvMutex;
    std::condition_variable m_cv;
};

} // namespace mediad
