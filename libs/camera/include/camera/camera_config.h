/**
 * @file camera_config.h
 * @brief 摄像头配置类（Builder 模式）
 *
 * CameraConfig 采用 Builder 模式构建配置，链式调用可读性强。
 * 构建完成后通过 validate() 校验合法性（分辨率对齐、参数范围）。
 *
 * Builder 持有独立成员变量而非 CameraConfig 对象，
 * 避免内部类中的不完整类型问题。
 */
#pragma once

#include "types.h"
#include "error.h"
#include <string>
#include <cstdint>

namespace camera {

/**
 * @brief 单通道编码配置
 */
struct ChannelConfig {
    uint16_t    width      = 1920;          ///< 宽度（必须为16的倍数）
    uint16_t    height     = 1080;          ///< 高度（必须为16的倍数）
    uint8_t     fps        = 30;            ///< 帧率（1-60）
    uint16_t    bitrate    = 2048;          ///< 目标码率（kbps）
    uint16_t    maxBitrate = 0;             ///< 最大码率（0=自动为bitrate*1.5）
    CodecType   codec      = CodecType::H264; ///< 编码格式
    BitrateMode brMode     = BitrateMode::VBR;///< 码率控制模式
    uint8_t     gopLen     = 30;            ///< GOP长度（I帧间隔，fps的倍数）
    uint8_t     minQP      = 20;            ///< 最小QP（画质上限，越小越好）
    uint8_t     maxQP      = 45;            ///< 最大QP（画质下限，越大越差）
    uint8_t     initQP     = 30;            ///< 初始QP
    uint8_t     frameDepth = 3;             ///< 帧缓冲深度（影响延迟与稳定性）
};

/**
 * @brief 摄像头完整配置
 *
 * 使用 Builder 模式构建，典型用法：
 * @code
 * auto config = CameraConfig::builder()
 *     .deviceId(0)
 *     .sensorConfig("/etc/isp_sensor.conf")
 *     .main(1920, 1080, 30, 2048)
 *     .sub(640, 480, 15, 512)
 *     .build();
 * @endcode
 */
class CameraConfig {
public:
    // --------------------------------------------------------
    // Builder 模式
    // --------------------------------------------------------

    /**
     * @brief Builder 类
     *
     * Builder 持有独立成员变量，build() 时组装为 CameraConfig。
     * 所有方法均在类体内 inline 定义，无链接问题。
     */
    class Builder {
    public:
        Builder() = default;

        Builder& deviceId(int id) {
            m_deviceId = id;
            return *this;
        }

        Builder& sensorConfig(const std::string& path) {
            m_sensorConfig = path;
            return *this;
        }

        Builder& main(uint16_t w, uint16_t h, uint8_t fps, uint16_t kbps) {
            m_mainChn.width   = w;
            m_mainChn.height  = h;
            m_mainChn.fps     = fps;
            m_mainChn.bitrate = kbps;
            return *this;
        }

        Builder& sub(uint16_t w, uint16_t h, uint8_t fps, uint16_t kbps) {
            m_subChn.width   = w;
            m_subChn.height  = h;
            m_subChn.fps     = fps;
            m_subChn.bitrate = kbps;
            return *this;
        }

        Builder& mainCodec(CodecType codec) {
            m_mainChn.codec = codec;
            return *this;
        }

        Builder& subCodec(CodecType codec) {
            m_subChn.codec = codec;
            return *this;
        }

        Builder& mainChannel(const ChannelConfig& cfg) {
            m_mainChn = cfg;
            return *this;
        }

        Builder& subChannel(const ChannelConfig& cfg) {
            m_subChn = cfg;
            return *this;
        }

        Builder& mainVIChnId(int id) {
            m_mainVIChnId = id;
            return *this;
        }

        Builder& subVIChnId(int id) {
            m_subVIChnId = id;
            return *this;
        }

        /**
         * @brief 构建并校验配置
         * @return 构建完成的 CameraConfig 对象
         * @throws CameraException 如果配置无效
         *
         * 自动填充逻辑：
         * - 主通道：如未调用 .main() 配置，自动填充最小默认值（640x480@1fps 128kbps）
         *   原因：部分平台（如 Anyka）启用设备时要求主通道必须存在，
         *   否则报错。自动填充的最小配置仅占用极少资源，不影响性能。
         */
        CameraConfig build() const;

    private:
        int           m_deviceId     = 0;
        /* 默认指向 Anyka AV100 的 sensor 配置，换平台时必须用 sensorConfig() 显式指定 */
        std::string   m_sensorConfig = "/etc/isp_sensor.conf";
        /* 平台视频输入通道号提示：默认 0/1 适配多数平台，不匹配时用同名 Builder 方法改 */
        int           m_mainVIChnId  = 0;
        int           m_subVIChnId   = 1;
        bool          m_subEnabled   = true;
        
        /* 主通道默认禁用（width=0 表示未配置）
         * 注意：build() 时会自动填充最小默认值（640x480@1fps），
         * 因为部分平台要求主通道必须存在。 */
        ChannelConfig m_mainChn = [] {
            ChannelConfig c;
            c.width   = 0;
            c.height  = 0;
            c.fps     = 0;
            c.bitrate = 0;
            return c;
        }();
        
        /* 子通道默认启用（640x480@15fps 512kbps） */
        ChannelConfig m_subChn = [] {
            ChannelConfig c;
            c.width   = 640;
            c.height  = 480;
            c.fps     = 15;
            c.bitrate = 512;
            return c;
        }();
    };

    /**
     * @brief 创建 Builder 实例
     */
    static Builder builder() { return Builder(); }

    // --------------------------------------------------------
    // 访问器
    // --------------------------------------------------------

    int                 deviceId()     const noexcept { return m_deviceId; }
    const std::string&  sensorConfig() const noexcept { return m_sensorConfig; }
    const ChannelConfig& mainChannel() const noexcept { return m_mainChn; }
    const ChannelConfig& subChannel()  const noexcept { return m_subChn; }
    int                 mainVIChnId()  const noexcept { return m_mainVIChnId; }
    int                 subVIChnId()   const noexcept { return m_subVIChnId; }
    bool                subEnabled()   const noexcept { return m_subEnabled; }

    /**
     * @brief 检查通道是否被用户显式配置
     * @param id 通道 ID
     * @return true 如果用户调用了 .main() 或 .sub()
     * 
     * 判断依据：width > 0 表示用户显式配置
     */
    bool isChannelConfigured(ChannelId id) const noexcept {
        if (id == ChannelId::Main) {
            return m_mainChn.width > 0;
        } else {
            return m_subChn.width > 0;
        }
    }

    // --------------------------------------------------------
    // 校验
    // --------------------------------------------------------

    /**
     * @brief 校验配置合法性
     * @throws CameraException 如果任一规则不满足
     */
    void validate() const;

private:
    // 设备参数
    int         m_deviceId     = 0;
    std::string m_sensorConfig = "/etc/isp_sensor.conf";
    int         m_mainVIChnId  = 0;
    int         m_subVIChnId   = 1;
    bool        m_subEnabled   = true;

    // 通道参数
    ChannelConfig m_mainChn;
    ChannelConfig m_subChn = [] {
        ChannelConfig c;
        c.width   = 640;
        c.height  = 480;
        c.fps     = 15;
        c.bitrate = 512;
        return c;
    }();
};

} // namespace camera
