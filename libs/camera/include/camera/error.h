/**
 * @file error.h
 * @brief 错误码与异常类型定义
 *
 * 定义库的所有错误码以及 CameraException 异常类。
 * 错误码采用枚举类，兼容整数返回值约定（0=成功，负数=失败）。
 */
#pragma once

#include <stdexcept>
#include <string>

namespace camera {

// ============================================================
// 错误码
// ============================================================

/**
 * @brief 操作结果错误码
 *
 * 设计约定：
 *   - 0       = 成功
 *   - 负数    = 各类错误
 *   - 正数    = 预留扩展
 */
enum class Error : int32_t {
    // --- 成功 ---
    Ok                   =  0,    ///< 操作成功

    // --- 通用错误 ---
    Unknown              = -1,    ///< 未知错误
    InvalidArgument      = -2,    ///< 无效参数（分辨率越界、空指针等）
    InvalidState         = -3,    ///< 状态不匹配（如未 start 就 stop）
    NotImplemented       = -4,    ///< 功能未实现
    Timeout              = -5,    ///< 操作超时

    // --- 设备错误 ---
    DeviceOpenFailed     = -10,   ///< 打开 VI 设备失败
    DeviceConfigFailed   = -11,   ///< 配置 VI 设备属性失败
    DeviceEnableFailed   = -12,   ///< 启用 VI 设备失败
    ChannelEnableFailed  = -13,   ///< 启用 VI 通道失败
    SensorConfigFailed   = -14,   ///< 加载 ISP/Sensor 配置失败
    ResolutionExceeded   = -15,   ///< 分辨率超出硬件上限
    ResolutionNotAligned = -16,   ///< 分辨率未对齐（需为16的倍数）
    ChannelBusy          = -17,   ///< 通道被订阅推流占用（拍照互斥硬约束：
                                  ///  SDK bind 内部线程独占 VI 帧队列，禁止并发抢帧）

    // --- 编码器错误 ---
    EncoderOpenFailed    = -20,   ///< 打开 VENC 编码器失败
    EncoderBindFailed    = -21,   ///< VI-VENC 绑定失败
    EncoderEncodeFailed  = -22,   ///< 编码帧失败
    EncoderNoData        = -23,   ///< 编码器无数据（正常情况，非错误）
    DmaMemoryInsufficient= -24,   ///< DMA 内存不足

    // --- 线程错误 ---
    ThreadCreateFailed   = -30,   ///< 创建采集线程失败

    // --- 资源错误 ---
    ResourceBusy         = -40,   ///< 资源被占用（如设备已被打开）
    MemoryAllocFailed    = -41,   ///< 内存分配失败
    DeviceBusy           = -42,   ///< 设备忙（正在重启中，不可用）
};

// ============================================================
// 辅助函数：将 Error 转换为字符串描述
// ============================================================

/**
 * @brief 将错误码转换为可读字符串
 * @param err 错误码
 * @return 错误描述字符串（静态常量，不需要释放）
 */
inline const char* errorToString(Error err) noexcept {
    switch (err) {
        case Error::Ok:                    return "Ok";
        case Error::Unknown:               return "Unknown error";
        case Error::InvalidArgument:       return "Invalid argument";
        case Error::InvalidState:          return "Invalid state";
        case Error::NotImplemented:        return "Not implemented";
        case Error::Timeout:               return "Timeout";
        case Error::DeviceOpenFailed:      return "Device open failed";
        case Error::DeviceConfigFailed:    return "Device config failed";
        case Error::DeviceEnableFailed:    return "Device enable failed";
        case Error::ChannelEnableFailed:   return "Channel enable failed";
        case Error::SensorConfigFailed:    return "Sensor config failed";
        case Error::ResolutionExceeded:    return "Resolution exceeds hardware limit";
        case Error::ResolutionNotAligned:  return "Resolution not 16-byte aligned";
        case Error::ChannelBusy:           return "Channel busy (streaming, snapshot rejected)";
        case Error::EncoderOpenFailed:     return "Encoder open failed";
        case Error::EncoderBindFailed:     return "Encoder bind failed";
        case Error::EncoderEncodeFailed:   return "Encoder encode failed";
        case Error::EncoderNoData:         return "Encoder no data";
        case Error::DmaMemoryInsufficient: return "DMA memory insufficient";
        case Error::ThreadCreateFailed:    return "Thread create failed";
        case Error::ResourceBusy:          return "Resource busy";
        case Error::MemoryAllocFailed:     return "Memory alloc failed";
        case Error::DeviceBusy:            return "Device busy (restarting)";
        default:                           return "Undefined error";
    }
}

// ============================================================
// 异常类
// ============================================================

/**
 * @brief 摄像头库异常基类
 *
 * 使用示例：
 * @code
 * try {
 *     CameraDevice cam(config);
 *     cam.start();
 * } catch (const CameraException& e) {
 *     LOG_ERROR("Camera error [{}]: {}", (int)e.code(), e.what());
 * }
 * @endcode
 */
class CameraException : public std::runtime_error {
public:
    /**
     * @brief 构造函数
     * @param code  错误码
     * @param msg   附加错误描述（可选）
     */
    explicit CameraException(Error code, const std::string& msg = "")
        : std::runtime_error(buildMessage(code, msg))
        , m_code(code) {}

    /**
     * @brief 获取错误码
     */
    Error code() const noexcept { return m_code; }

private:
    Error m_code;

    /**
     * @brief 拼接错误消息：[ErrorName] msg
     */
    static std::string buildMessage(Error code, const std::string& msg) {
        std::string result = "[";
        result += errorToString(code);
        result += "]";
        if (!msg.empty()) {
            result += " ";
            result += msg;
        }
        return result;
    }
};

} // namespace camera
