/**
 * @file camera_config_impl.cpp
 * @brief CameraConfig::validate() 和 Builder::build() 实现
 */
#include "camera/camera_config.h"
#include "camera/error.h"

#include <sstream>

namespace camera {

/* ---------------------------------------------------------------
 * Builder::build() — 组装 CameraConfig 并校验
 * --------------------------------------------------------------- */
/**
 * @brief 组装 CameraConfig 并校验
 * @return 构建完成的 CameraConfig 对象
 *
 * 自动填充逻辑：
 * - 主通道：如未配置（width=0），自动填充最小默认值（640x480@1fps 128kbps）
 *   原因：部分平台（如 Anyka）启用设备时要求主通道必须存在，
 *   否则报错。填充的最小配置仅占用极少资源，不影响性能。
 */
CameraConfig CameraConfig::Builder::build() const {
    CameraConfig cfg;
    cfg.m_deviceId     = m_deviceId;
    cfg.m_sensorConfig = m_sensorConfig;
    cfg.m_mainVIChnId  = m_mainVIChnId;
    cfg.m_subVIChnId   = m_subVIChnId;
    cfg.m_subEnabled   = m_subEnabled;
    
    /* 自动填充主通道默认值（平台约束：主通道必须存在） */
    if (m_mainChn.width == 0) {
        cfg.m_mainChn.width   = 640;
        cfg.m_mainChn.height  = 480;
        cfg.m_mainChn.fps     = 1;
        cfg.m_mainChn.bitrate = 128;
        cfg.m_mainChn.codec   = CodecType::H264;
        cfg.m_mainChn.brMode  = BitrateMode::VBR;
        cfg.m_mainChn.gopLen  = 30;
        cfg.m_mainChn.minQP   = 20;
        cfg.m_mainChn.maxQP   = 45;
        cfg.m_mainChn.initQP  = 30;
        cfg.m_mainChn.frameDepth = 3;
    } else {
        cfg.m_mainChn = m_mainChn;
    }
    
    cfg.m_subChn = m_subChn;
    cfg.validate();
    return cfg;
}

/* ---------------------------------------------------------------
 * CameraConfig::validate() — 配置合法性校验
 * --------------------------------------------------------------- */

void CameraConfig::validate() const {
    /* --- 主通道校验（仅在启用时） --- */
    if (m_mainChn.width > 0) {
        /* 宽度已设置，检查高度 */
        if (m_mainChn.height == 0) {
            throw CameraException(Error::InvalidArgument,
                                  "main channel height must be non-zero when width is set");
        }
        /* YUV420 格式要求宽高均为偶数（2 像素对齐） */
        if (m_mainChn.width % 2 != 0 || m_mainChn.height % 2 != 0) {
            std::ostringstream oss;
            oss << "main channel resolution " << m_mainChn.width << "x" << m_mainChn.height
                << " must be even numbers";
            throw CameraException(Error::InvalidArgument, oss.str());
        }
        if (m_mainChn.fps < 1 || m_mainChn.fps > 60) {
            std::ostringstream oss;
            oss << "main channel fps " << (int)m_mainChn.fps << " out of range [1, 60]";
            throw CameraException(Error::InvalidArgument, oss.str());
        }
        if (m_mainChn.gopLen == 0) {
            throw CameraException(Error::InvalidArgument,
                                  "main channel gopLen must be >= 1");
        }
    }

    /* --- 子通道校验（仅在启用时） --- */
    if (m_subEnabled) {
        if (m_subChn.width == 0 || m_subChn.height == 0) {
            throw CameraException(Error::InvalidArgument,
                                  "sub channel resolution must be non-zero");
        }
        /* YUV420 格式要求宽高均为偶数（2 像素对齐） */
        if (m_subChn.width % 2 != 0 || m_subChn.height % 2 != 0) {
            std::ostringstream oss;
            oss << "sub channel resolution " << m_subChn.width << "x" << m_subChn.height
                << " must be even numbers";
            throw CameraException(Error::InvalidArgument, oss.str());
        }
        /* Anyka 硬件硬限：子通道最大 1280x1024 */
        if (m_subChn.width > 1280 || m_subChn.height > 1024) {
            std::ostringstream oss;
            oss << "sub channel resolution " << m_subChn.width << "x" << m_subChn.height
                << " exceeds hardware limit 1280x1024";
            throw CameraException(Error::InvalidArgument, oss.str());
        }
        if (m_subChn.fps < 1 || m_subChn.fps > 60) {
            std::ostringstream oss;
            oss << "sub channel fps " << (int)m_subChn.fps << " out of range [1, 60]";
            throw CameraException(Error::InvalidArgument, oss.str());
        }
        if (m_subChn.gopLen == 0) {
            throw CameraException(Error::InvalidArgument,
                                  "sub channel gopLen must be >= 1");
        }
    }

    /* --- Sensor 配置路径 --- */
    if (m_sensorConfig.empty()) {
        throw CameraException(Error::InvalidArgument,
                              "sensorConfig path must not be empty");
    }
}

} // namespace camera
