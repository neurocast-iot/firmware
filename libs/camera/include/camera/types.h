/**
 * @file types.h
 * @brief 摄像头库核心类型定义
 *
 * 本文件定义库的公共类型，所有上层代码通过此文件统一引用类型。
 * 不包含任何 Anyka SDK 头文件，保持接口层清洁。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "error.h"  // Error 枚举

namespace camera {

// ============================================================
// 参数应用结果
// ============================================================

/**
 * @brief 配置应用结果
 */
struct ApplyResult {
    bool success;           ///< 是否成功
    bool restarted;         ///< 是否发生了重启
    Error error;            ///< 失败时的错误码
    std::string errorMsg;   ///< 失败详情
};

// ============================================================
// 编码格式
// ============================================================

/**
 * @brief 视频编码类型
 */
enum class CodecType : uint8_t {
    H264  = 0,   ///< H.264/AVC
    H265  = 1,   ///< H.265/HEVC
    MJPEG = 2,   ///< Motion JPEG
};

/** 字符串转编码枚举（"h264"→H264，"h265"→H265，其余默认 H264） */
inline CodecType parseCodecType(const std::string& s) {
    if (s == "h265" || s == "H265") return CodecType::H265;
    return CodecType::H264;
}

// ============================================================
// 帧类型
// ============================================================

/**
 * @brief 视频帧类型
 */
enum class FrameType : uint8_t {
    P   = 0,   ///< P帧（预测帧）
    I   = 1,   ///< I帧（关键帧/IDR）
    PI  = 2,   ///< PI帧（渐进式刷新帧，Anyka特有）
};

// ============================================================
// 通道标识
// ============================================================

/**
 * @brief 通道类型
 */
enum class ChannelId : uint8_t {
    Main = 0,   ///< 主通道（高分辨率，用于推流）
    Sub  = 1,   ///< 子通道（低分辨率，用于录制/预览）
};

// ============================================================
// 设备状态
// ============================================================

/**
 * @brief 摄像头设备状态
 */
enum class DeviceState : uint8_t {
    Idle       = 0,   ///< 未初始化
    Configured = 1,   ///< 已配置，未启动
    Running    = 2,   ///< 正在采集
    Error      = 3,   ///< 发生错误
    Stopping   = 4,   ///< 正在停止
};

// ============================================================
// 码率控制模式
// ============================================================

/**
 * @brief 码率控制模式（映射自 Anyka venc_rc_param）
 */
enum class BitrateMode : uint8_t {
    CBR  = 0,   ///< 固定码率
    VBR  = 1,   ///< 可变码率（推荐：画质稳定）
    AVBR = 2,   ///< 自适应 VBR
};

/** 字符串转码率模式枚举（"cbr"→CBR，"avbr"→AVBR，其余默认 VBR） */
inline BitrateMode parseBitrateMode(const std::string& s) {
    if (s == "cbr" || s == "CBR") return BitrateMode::CBR;
    if (s == "avbr" || s == "AVBR") return BitrateMode::AVBR;
    return BitrateMode::VBR;
}

// ============================================================
// 白天/夜间模式
// ============================================================

/**
 * @brief ISP 日夜模式
 */
enum class DayNightMode : uint8_t {
    Day       = 0,   ///< 白天室外模式
    Night     = 1,   ///< 夜间模式
    Auto      = 2,   ///< 自动切换（ISP决策）
};

} // namespace camera
