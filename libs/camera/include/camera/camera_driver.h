/**
 * @file camera_driver.h
 * @brief 摄像头驱动抽象接口（瘦接口，只暴露硬件操作）
 *
 * CameraDriver 是所有摄像头 SDK 的公共接口。
 * 每个平台（Anyka、海思、瑞昱等）实现这个接口，上层代码不关心底层用的什么 SDK。
 *
 * 设计原则：
 *   - 驱动层只做 SDK 调用映射，不包含业务逻辑
 *   - 拍照编排（JPEG 编码 + 写文件）在 CameraDevice::Impl 编排层
 *   - 配置应用（diff + 重启决策）在 CameraDevice::Impl 编排层
 *   - 接入新平台只需实现本接口的 SDK 调用，不碰业务逻辑
 *
 * 接入新摄像头只需：
 *   1. 新建 src/xxx/xxx_driver.h + xxx_driver.cpp
 *   2. 实现 CameraDriver 所有虚函数（只映射 SDK 调用）
 *   3. CameraDevice::Impl 构造时选择对应 Driver
 *   4. 上层 Service 代码零修改
 */
#pragma once

#include "camera_config.h"
#include "error.h"
#include "types.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace camera {

// 前向声明，避免循环依赖
class EventDispatcher;

/* ================================================================
 * 平台无关的原始帧结构
 * ================================================================ */

/**
 * @brief 原始 YUV 帧数据
 *
 * 驱动层通过 acquireFrame() 填充，编排层读取 data/len 做编码。
 * platformHandle 由驱动内部使用（释放帧时需要），上层不碰。
 */
struct RawFrame {
    const uint8_t* data   = nullptr;  ///< YUV 帧数据指针
    size_t         len    = 0;        ///< 帧数据长度（字节）
    int            width  = 0;        ///< 帧宽度
    int            height = 0;        ///< 帧高度
    void*          platformHandle = nullptr;  ///< 驱动内部句柄（上层不碰）
};

/* ================================================================
 * 通道显式状态
 * ================================================================ */

/**
 * @brief 通道状态枚举
 *
 * 每个通道在任意时刻处于且仅处于以下一个状态：
 *
 *   Idle        通道对象不存在，没有任何 SDK 资源
 *   Configured  已注册通道属性（set_chn_attr），但 VI 通道未启用
 *   Enabled     VI 通道已启用（enable_chn），可以取帧，但没有推流
 *   Streaming   推流中：有编码器 + 采集线程，帧通过 EventDispatcher 推送
 *   Error       出错，需要重建
 */
enum class ChannelState : uint8_t {
    Idle,
    Configured,
    Enabled,
    Streaming,
    Error
};

/**
 * @brief 通道状态名（日志用）
 */
inline const char* channelStateName(ChannelState s) {
    switch (s) {
        case ChannelState::Idle:       return "Idle";
        case ChannelState::Configured: return "Configured";
        case ChannelState::Enabled:    return "Enabled";
        case ChannelState::Streaming:  return "Streaming";
        case ChannelState::Error:      return "Error";
    }
    return "Unknown";
}

/* ================================================================
 * 硬件能力
 * ================================================================ */

/**
 * @brief 驱动能力描述
 *
 * 不同 SDK 能力不同：
 * - Anyka: needRestartForResolution = true（改分辨率必须重启设备）
 * - 海思:  needRestartForResolution = false（支持运行时改分辨率）
 */
struct DriverCapabilities {
    int  mainMaxWidth  = 0;
    int  mainMaxHeight = 0;
    int  subMaxWidth   = 0;
    int  subMaxHeight  = 0;
    bool needRestartForResolution = true;
};

/* ================================================================
 * 移动侦测相关类型（平台无关）
 * ================================================================ */

/** 移动侦测配置 */
struct MotionDetectConfig {
    int sensitivity = 80;
    int confirmFrames = 1;
    int intervalMs = 200;
    int dayNightMode = 1;
};

/** 运动区域（坐标基于 MD 矩阵，非像素） */
struct MotionRegion {
    int left;
    int top;
    int right;
    int bottom;
};

/** 运动事件回调 */
using MotionCallback = std::function<void(bool motion, const MotionRegion* regions, int count)>;

/* ================================================================
 * CameraDriver 接口
 * ================================================================ */

/**
 * @brief 摄像头驱动纯虚接口（瘦接口，只暴露硬件操作）
 *
 * 驱动层只做 SDK 调用映射：
 *   - 设备生命周期（init/cleanup/restart）
 *   - 通道 VI 控制（enableVI/disableVI）
 *   - 帧获取（acquireFrame/releaseFrame/drainStaleFrames）
 *   - 推流控制（startStream/stopStream）
 *   - 运行时控制（setFps/setBitrate/requestKeyFrame/setDayNightMode）
 *
 * 不包含：
 *   - 拍照编排（JPEG 编码 + 写文件）→ CameraDevice::Impl
 *   - 配置应用（diff + 重启决策）→ CameraDevice::Impl
 */
class CameraDriver {
public:
    using LogCallback = std::function<void(int level, const std::string& msg)>;

    virtual ~CameraDriver() = default;

    /* ---- 设备生命周期 ---- */

    /**
     * @brief 初始化设备
     *
     * 完整流程：打开设备 → 加载 sensor 配置 → 注册通道 → 应用配置 → 启用设备。
     * 完成后所有通道处于 Configured 状态。
     */
    virtual Error init() = 0;

    /**
     * @brief 清理设备资源
     *
     * 完整流程：停所有推流 → 禁用所有通道 → 关闭设备。
     * 完成后所有通道回到 Idle 状态。
     */
    virtual void cleanup() = 0;

    /**
     * @brief 重启设备（用当前配置重新初始化）
     *
     * 等于 cleanup() + init()。改分辨率后必须调用。
     */
    virtual Error restart() = 0;

    /** @brief 设备是否正在运行 */
    virtual bool isRunning() const = 0;

    /* ---- 通道 VI 控制 ---- */

    /**
     * @brief 启用通道的视频输入（VI），之后可以取帧/推流
     *
     * 通道状态：Configured → Enabled
     * 启用后可以取帧（acquireFrame），但没有推流。
     *
     * 平台约束（如 Anyka）：启用后必须紧跟取帧，不能长时间放着不管。
     */
    virtual Error enableVI(ChannelId ch) = 0;

    /**
     * @brief 禁用通道的视频输入（VI）
     *
     * 通道状态：Enabled → Configured
     */
    virtual void disableVI(ChannelId ch) = 0;

    /* ---- 帧获取（拍照用） ---- */

    /**
     * @brief 获取一帧原始 YUV 数据
     *
     * 通道必须处于 Enabled 或 Streaming 状态。
     * 获取后必须用 releaseFrame() 释放。
     */
    virtual Error acquireFrame(ChannelId ch, RawFrame& frame) = 0;

    /**
     * @brief 释放帧资源
     *
     * 必须与 acquireFrame() 配对使用。
     */
    virtual void releaseFrame(ChannelId ch, RawFrame& frame) = 0;

    /**
     * @brief 排空视频输入队列中积压的旧帧
     *
     * 拍照前先排空，再 acquireFrame() 取到的才是当下画面。
     * @param ch 目标通道
     * @param maxCount 最多排空帧数（传通道 frameDepth 即可）
     */
    virtual Error drainStaleFrames(ChannelId ch, int maxCount) = 0;

    /**
     * @brief 获取硬件编码器的最新 JPEG 数据（拍照用）
     *
     * 驱动内部有后台线程持续从 JPEG 编码器取帧，
     * 调用时直接返回最新一帧，不需要等视频输入出帧。
     * @param ch 目标通道（通常用子通道）
     * @param jpegData 输出：编码好的 JPEG 数据
     */
    virtual Error captureJpeg(ChannelId ch, std::vector<uint8_t>& jpegData) { return Error::NotImplemented; }

    /**
     * @brief 按需把一帧 YUV 编码成 JPEG（拍照回退路用）
     *
     * 平台没有常驻 JPEG 编码器（captureJpeg 返回 NotImplemented）时，
     * 编排层取到 YUV 帧后走这里临时编码。不支持任何编码能力的平台保持默认实现。
     * @param ch 帧来源通道（用于确定分辨率）
     * @param yuvData/yuvLen acquireFrame 取到的 YUV 原始帧数据
     * @param quality JPEG 质量（1-100）
     * @param jpegOut 输出 JPEG 数据
     * @return NotImplemented 表示平台完全不支持按需编码，拍照回退路不可用
     */
    virtual Error encodeRawToJpeg(ChannelId ch, const uint8_t* yuvData, size_t yuvLen,
                                  int quality, std::vector<uint8_t>& jpegOut) {
        (void)ch; (void)yuvData; (void)yuvLen; (void)quality; (void)jpegOut;
        return Error::NotImplemented;
    }

    /**
     * @brief 获取最新一帧缩略图 JPEG 数据
     *
     * 和 captureJpeg 走同一层级，但读的是缩略图编码器的 buffer。
     * 缩略图由硬件缩放后编码，尺寸以 startThumbnail 入参为准。
     * @param thumbData 输出：缩略图 JPEG 数据
     */
    virtual Error captureThumbnail(std::vector<uint8_t>& thumbData) { return Error::NotImplemented; }

    /**
     * @brief 启动缩略图编码器（创建 JPEG 编码器 + 绑定 + 后台线程）
     * @param width  缩略图宽度
     * @param height 缩略图高度
     */
    virtual Error startThumbnail(uint16_t width, uint16_t height) { return Error::NotImplemented; }

    /**
     * @brief 停止缩略图编码器
     */
    virtual void stopThumbnail() {}

    /**
     * @brief 获取缩略图帧序号（录像缩略图守护用）
     * @return 帧序号（从 1 开始，0 表示还没有帧）
     */
    virtual uint64_t getThumbFrameSeq() const { return 0; }

    /* ---- 移动侦测 ---- */

    /**
     * @brief 启动硬件移动侦测（硬件统计 + 侦测算法，具体实现由平台决定）
     *
     * 录像期间旁路检测画面变化，零 CPU 开销，不占视频输入帧。
     * 检测到运动后回调 onMotion，调用方可据此抓缩略图/触发录像。
     *
     * @param cfg    检测参数（灵敏度、确认帧数、检测周期）
     * @param onMotion 运动回调（在检测线程中同步调用，回调内不做耗时操作）
     * @return Error::Ok 成功，Error::NotImplemented 平台不支持
     */
    virtual Error startMotionDetection(const MotionDetectConfig& cfg,
                                       MotionCallback onMotion) {
        (void)cfg; (void)onMotion;
        return Error::NotImplemented;
    }

    /**
     * @brief 停止移动侦测
     *
     * 幂等：未启动时调用无副作用。
     */
    virtual void stopMotionDetection() {}

    /* ---- 推流控制（订阅用） ---- */

    /**
     * @brief 启动推流
     *
     * 完整流程：启用通道 → 创建编码器 → 绑定视频输入到编码器 → 启动采集线程。
     * 通道状态：Configured/Enabled → Streaming
     */
    virtual Error startStream(ChannelId ch) = 0;

    /**
     * @brief 停止推流
     *
     * 完整流程：停止采集线程 → 解绑 → 关闭编码器 → 禁用通道。
     * 通道状态：Streaming → Configured
     */
    virtual void stopStream(ChannelId ch) = 0;

    /* ---- 拍照控制（按需启停） ---- */

    /**
     * @brief 启动拍照能力（创建 JPEG 编码器 + 绑定 + 后台取帧线程）
     *
     * 有拍照触发源时调用，和 startStream 一样按需启动。
     * 没有拍照触发源时不启动，不浪费 DMA 内存和 CPU。
     */
    virtual Error startSnapshot() { return Error::NotImplemented; }

    /**
     * @brief 停止拍照能力（停线程 → 解绑 → 关编码器）
     *
     * 拍照触发源全部移除时调用，释放 JPEG 编码资源。
     * 视频输入子通道保持开着（和 stopStream 后保持一致）。
     */
    virtual void stopSnapshot() {}

    /* ---- 运行时控制 ---- */

    /** @brief 动态设置帧率（热更新，不需要重启） */
    virtual Error setFps(ChannelId ch, uint8_t fps) = 0;

    /** @brief 动态设置码率（热更新，不需要重启） */
    virtual Error setBitrate(ChannelId ch, uint16_t kbps) = 0;

    /** @brief 请求强制输出 IDR 帧（关键帧） */
    virtual Error requestKeyFrame(ChannelId ch) = 0;

    /** @brief 切换日夜模式 */
    virtual Error setDayNightMode(DayNightMode mode) = 0;

    /* ---- 查询 ---- */

    /** @brief 查询通道当前状态 */
    virtual ChannelState getChannelState(ChannelId ch) const = 0;

    /** @brief 获取硬件能力（最大分辨率、是否需要重启等） */
    virtual DriverCapabilities getCapabilities() const = 0;

    /** @brief 获取当前配置 */
    virtual const CameraConfig& config() const = 0;

    /**
     * @brief 更新配置
     *
     * 编排层在 applyConfig() 时调用，驱动层保存新配置副本。
     * restart() 时使用更新后的配置重新初始化。
     */
    virtual void updateConfig(const CameraConfig& cfg) = 0;

    /* ---- 基础设施 ---- */

    /** @brief 注册日志回调 */
    virtual void setLogCallback(LogCallback cb) = 0;

    /**
     * @brief 绑定帧分发器
     *
     * 驱动内部采集线程拿到帧后，通过 dispatcher 推送给所有消费者。
     * 在 init() 之前调用。
     */
    virtual void setDispatcher(EventDispatcher* dispatcher) = 0;
};

} // namespace camera
