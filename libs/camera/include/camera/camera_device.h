/**
 * @file camera_device.h
 * @brief 摄像头设备核心类（对外主入口）
 *
 * CameraDevice 是库的唯一对外入口类，封装了：
 *   - 设备生命周期（RAII：析构自动释放所有 SDK 资源）
 *   - 异步帧获取（观察者模式：subscribe/unsubscribe）
 *   - 运行时动态调整（帧率、码率、IDR 帧请求）
 *   - 编码器动态休眠/唤醒（节省 CPU 资源）
 *   - 快照功能（JPEG 拍照）
 *
 * Pimpl 惯用法隐藏所有 Anyka SDK 依赖，使上层代码无需包含任何 SDK 头文件。
 *
 * 设计参考：
 *   - libcamera Camera 类
 *   - Android Camera2 CameraDevice
 */
#pragma once

#include "camera_config.h"
#include "camera_driver.h"
#include "event_dispatcher.h"
#include "error.h"
#include "types.h"
#include <memory>
#include <string>
#include <functional>

namespace camera {

/**
 * @brief 摄像头设备
 *
 * 使用示例：
 * @code
 * // 1. 构建配置
 * auto config = CameraConfig::builder()
 *     .deviceId(0)
 *     .sensorConfig("/etc/isp_sensor.conf")
 *     .main(1920, 1080, 30, 2048)
 *     .sub(640, 480, 15, 512)
 *     .build();
 *
 * // 2. 创建设备（RAII）
 * CameraDevice camera(config);
 *
 * // 3. 订阅主通道帧
 * auto subId = camera.subscribe(ChannelId::Main, [](const VideoFrame& frame) {
 *     if (frame.isKeyFrame()) { ... }
 *     sendRtmp(frame.data(), frame.size(), frame.timestamp());
 * });
 *
 * // 4. 启动采集
 * camera.start();
 *
 * // 5. 运行中动态调整配置
 * camera.applyConfig(CameraConfig::builder()
 *     .main(1920, 1080, 25, 3072)
 *     .build());
 *
 * // 6. 取消订阅
 * camera.unsubscribe(subId);
 *
 * // 7. 停止（析构时自动执行）
 * camera.stop();
 * @endcode
 */
class CameraDevice {
public:
    // --------------------------------------------------------
    // 构造与析构
    // --------------------------------------------------------

    /**
     * @brief 构造函数（不初始化硬件，仅保存配置）
     * @param config 摄像头配置（通过 CameraConfig::builder() 构建）
     */
    explicit CameraDevice(const CameraConfig& config);

    /**
     * @brief 析构函数（自动调用 stop() 并释放所有 SDK 资源）
     */
    ~CameraDevice();

    // 禁止拷贝和移动（持有独占硬件资源）
    CameraDevice(const CameraDevice&)            = delete;
    CameraDevice& operator=(const CameraDevice&) = delete;
    CameraDevice(CameraDevice&&)                 = delete;
    CameraDevice& operator=(CameraDevice&&)      = delete;

    // --------------------------------------------------------
    // 生命周期管理
    // --------------------------------------------------------

    /**
     * @brief 启动摄像头采集
     *
     * 执行步骤：
     *   1. 打开 VI 设备并加载 Sensor 配置
     *   2. 配置 VI 设备属性（分辨率、帧率、ISP路径）
     *   3. 创建并配置 VENC 编码器
     *   4. 绑定 VI 通道到 VENC（内核态编码，零拷贝）
     *   5. 启用 VI 设备和通道
     *   6. 启动采集线程（循环获取编码后的码流并分发给订阅者）
     *
     * @return Error::Ok 成功；其他值见 error.h
     * @throws CameraException（配置无效时）
     */
    Error start();

    /**
     * @brief 停止摄像头采集
     *
     * 执行步骤：
     *   1. 设置停止标志，等待采集线程退出
     *   2. 解绑 VI-VENC 通道
     *   3. 关闭 VENC 编码器
     *   4. 禁用 VI 通道和设备
     *   5. 关闭 VI 设备
     *
     * 幂等操作：未启动时调用无副作用。
     */
    void stop();

    // --------------------------------------------------------
    // 观察者订阅
    // --------------------------------------------------------

    /**
     * @brief 订阅指定通道的视频帧
     *
     * @param channel  目标通道（Main 或 Sub）
     * @param callback 帧到达回调（在采集线程中同步调用）
     * @return         订阅 ID（用于 unsubscribe），失败返回 kInvalidSubscriptionId
     *
     * 注意：
     *   - 可在 start() 之前或之后调用
     *   - 同一通道支持多个订阅者
     *   - 回调执行期间帧数据有效，回调返回后不得继续使用
     */
    SubscriptionId subscribe(ChannelId channel, FrameCallback callback);

    /**
     * @brief 取消订阅
     *
     * @param id 由 subscribe() 返回的订阅 ID
     *
     * 线程安全：可在任意线程调用，包括回调内部。
     */
    void unsubscribe(SubscriptionId id);

    // --------------------------------------------------------
    // 拍照能力启停（按需，和推流订阅一样）
    // --------------------------------------------------------

    /**
     * @brief 启动拍照能力（创建 VENC JPEG 编码器 + 绑定 + 后台线程）
     *
     * 有拍照触发源时调用，没有时不调用，不浪费 DMA 内存和 CPU。
     * 幂等：已启动时重复调用无副作用。
     */
    Error startSnapshot();

    /**
     * @brief 停止拍照能力（停线程 → 解绑 → 关编码器）
     *
     * 拍照触发源全部移除时调用。VI 子通道保持开着。
     * 幂等：未启动时调用无副作用。
     */
    void stopSnapshot();

    // --------------------------------------------------------
    // 运行时调整
    // --------------------------------------------------------

    /**
     * @brief 检查新配置是否会导致编码器重启
     *
     * 内部自动识别哪些通道需要更新（width > 0 表示需要更新），
     * 检查分辨率是否变化。
     *
     * @param newConfig 新配置（可包含主通道、子通道或两者）
     * @return true 任一通道会重启（分辨率变化），false 仅热更新（fps/kbps）
     */
    bool checkWillRestart(const CameraConfig& newConfig) const;

    /**
     * @brief 应用新配置（内部自动选择热更新或重启）
     *
     * 内部逻辑：
     *   - 自动识别需要更新的通道（width > 0）
     *   - 分辨率变化 → 重启编码器
     *   - 仅 fps/kbps 变化 → 热更新
     *
     * @param newConfig 新配置
     * @return ApplyResult 应用结果
     */
    ApplyResult applyConfig(const CameraConfig& newConfig);

    /**
     * @brief 请求强制输出关键帧（IDR帧）
     *
     * 典型场景：网络重连后立即发送 IDR 帧，避免花屏。
     *
     * @param channel 目标通道
     * @return Error::Ok 成功
     */
    Error requestKeyFrame(ChannelId channel);

    // --------------------------------------------------------
    // 编码器休眠/唤醒
    // --------------------------------------------------------

    /**
     * @brief 设置编码器活跃状态
     *
     * 休眠时：编码器停止工作，节省 CPU 和 DMA 内存。
     * 唤醒时：编码器重新启动，需 1-2 秒获得首帧 I 帧。
     *
     * 典型场景：无消费者时休眠节省功耗，有消费者时唤醒。
     *
     * @param channel 目标通道
     * @param active  true=唤醒，false=休眠
     * @return Error::Ok 成功
     */
    Error setEncoderActive(ChannelId channel, bool active);

    /**
     * @brief 查询编码器活跃状态
     *
     * @param channel 目标通道
     * @return true=活跃，false=休眠
     */
    bool isEncoderActive(ChannelId channel) const;

    // --------------------------------------------------------
    // 快照功能
    // --------------------------------------------------------

    /**
     * @brief 拍照（JPEG 格式输出到文件）
     *
     * 从指定 VI 通道捕获一帧 YUV 数据并编码为 JPEG 文件。
     * 不影响主/子通道的正常编码流程。
     *
     * @param channel     源通道（从哪个通道抓取原始帧）
     * @param outputPath  输出 JPEG 文件路径
     * @param quality     JPEG 质量（1-100，默认 80）
     * @return Error::Ok 成功
     */
    Error captureSnapshot(ChannelId channel,
                          const std::string& outputPath,
                          int quality = 80);

    /**
     * @brief 获取最新一帧缩略图 JPEG 数据
     *
     * 缩略图由独立的 VENC JPEG 编码器（320x176）后台持续生成，
     * 通过 VENC 硬件 frame_Resize 从子通道缩放后编码。
     * 调用时直接从 buffer 读，不需要等 VI 出帧。
     *
     * @param thumbData 输出：缩略图 JPEG 数据
     * @return Error::Ok 成功，Error::EncoderNoData 还没有帧
     */
    Error captureThumbnail(std::vector<uint8_t>& thumbData);

    /**
     * @brief 启动缩略图编码器
     *
     * 和 startSnapshot() 一起调用，创建独立的 VENC JPEG 编码器绑定到子通道。
     * @param width  缩略图宽度
     * @param height 缩略图高度
     */
    Error startThumbnail(uint16_t width, uint16_t height);

    /**
     * @brief 停止缩略图编码器
     *
     * 和 stopSnapshot() 一起调用，释放 VENC 资源。
     */
    void stopThumbnail();

    /**
     * @brief 获取缩略图帧序号（录像缩略图守护用）
     * @return 帧序号（从 1 开始，0 表示还没有帧）
     */
    uint64_t getThumbFrameSeq() const;

    // --------------------------------------------------------
    // 移动侦测
    // --------------------------------------------------------

    /**
     * @brief 启动硬件移动侦测
     *
     * 录像期间旁路检测画面变化，零 CPU 开销，不占 VI 帧。
     * 检测到运动后回调 onMotion。
     *
     * @param cfg 检测参数（灵敏度、确认帧数、检测周期）
     * @param onMotion 运动回调（在检测线程中同步调用，回调内不做耗时操作）
     * @return Error::Ok 成功，Error::NotImplemented 平台不支持
     */
    Error startMotionDetection(const MotionDetectConfig& cfg, MotionCallback onMotion);

    /**
     * @brief 停止移动侦测
     *
     * 幂等：未启动时调用无副作用。
     */
    void stopMotionDetection();

    // --------------------------------------------------------
    // ISP 控制
    // --------------------------------------------------------

    /**
     * @brief 切换日夜模式
     *
     * @param mode 目标模式（Day/Night/Auto）
     * @return Error::Ok 成功
     */
    Error setDayNightMode(DayNightMode mode);

    // --------------------------------------------------------
    // 状态查询
    // --------------------------------------------------------

    /**
     * @brief 获取设备当前状态
     */
    DeviceState state() const noexcept;

    /**
     * @brief 是否正在运行
     */
    bool isRunning() const noexcept;

    /**
     * @brief 获取当前配置（只读）
     */
    const CameraConfig& config() const noexcept;

    /**
     * @brief 获取硬件最大分辨率能力
     *
     * @param mainMaxW  主通道最大宽度（输出）
     * @param mainMaxH  主通道最大高度（输出）
     * @param subMaxW   子通道最大宽度（输出）
     * @param subMaxH   子通道最大高度（输出）
     * @return Error::Ok 成功
     */
    Error getHardwareCapability(int& mainMaxW, int& mainMaxH,
                                int& subMaxW,  int& subMaxH) const;

    // --------------------------------------------------------
    // 日志回调注入
    // --------------------------------------------------------

    /**
     * @brief 日志回调函数类型
     *
     * @param level 日志级别（0=ERROR, 1=WARN, 2=INFO, 3=DEBUG）
     * @param msg   日志消息
     */
    using LogCallback = std::function<void(int level, const std::string& msg)>;

    /**
     * @brief 注册日志回调
     *
     * 将库内部日志重定向到上层日志系统（如 spdlog）。
     * 未注册时输出到 stdout/stderr。
     *
     * @param callback 日志回调函数（nullptr=使用默认输出）
     */
    void setLogCallback(LogCallback callback);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;  ///< Pimpl（隐藏所有 SDK 依赖）
};

} // namespace camera
