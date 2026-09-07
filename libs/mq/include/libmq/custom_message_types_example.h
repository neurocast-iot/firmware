/**
 * custom_message_types_example.h
 * 自定义消息类型示例
 *
 * 本文件展示如何在应用层定义自己的消息类型，并使用 lib-mq 的通用接口进行通信。
 *
 * 消息类型范围分配：
 * - 0-9999:       lib-mq 库内置消息类型（不可修改）
 * - 10000-65535:  用户自定义消息类型
 *
 * 建议每个应用分配 10000 个类型范围，避免冲突：
 * - 应用 A: 10000-19999
 * - 应用 B: 20000-29999
 * - 应用 C: 30000-39999
 * - ...
 */
#pragma once
#include <cstdint>

namespace libmq {

// ===== 应用 A：安防监控系统 =====
namespace security_app {

/**
 * 安防监控进程内消息类型
 *
 * 使用范围：10000-10999
 * 用于摄像头模块与业务模块间的进程内通信
 */
enum class SecurityInprocMessageType : uint32_t {
    MOTION_DETECTED = 10000,    // 移动侦测告警
    FACE_RECOGNIZED = 10001,    // 人脸识别完成
    INTRUSION_ALERT = 10002,    // 入侵告警
    LINE_CROSSED = 10003,       // 越线检测告警
    ZONE_ENTERED = 10004,       // 区域进入告警
};

/**
 * 安防监控进程间消息类型
 *
 * 使用范围：11000-11999
 * 用于安防监控进程与 iot_live 主进程间的通信
 */
enum class SecurityIpcMessageType : uint32_t {
    ALARM_REPORT = 11000,       // 告警上报到主进程
    CAMERA_CONTROL = 11001,     // 摄像头控制命令
    SNAPSHOT_REQUEST = 11002,   // 抓拍请求
    VIDEO_CLIP_REQUEST = 11003, // 录像片段请求
};

}

// ===== 应用 B：智能门锁 =====
namespace smart_lock_app {

/**
 * 智能门锁进程内消息类型
 *
 * 使用范围：20000-20999
 */
enum class LockInprocMessageType : uint32_t {
    DOOR_OPENED = 20000,        // 门已打开
    DOOR_CLOSED = 20001,        // 门已关闭
    FINGERPRINT_MATCHED = 20002,// 指纹匹配成功
    PASSWORD_VERIFIED = 20003,  // 密码验证成功
    LOW_BATTERY = 20004,        // 电池电量低
};

/**
 * 智能门锁进程间消息类型
 *
 * 使用范围：21000-21999
 */
enum class LockIpcMessageType : uint32_t {
    LOCK_STATUS = 21000,        // 门锁状态上报
    UNLOCK_COMMAND = 21001,     // 远程开锁命令
    LOCK_COMMAND = 21002,       // 远程上锁命令
    ACCESS_LOG = 21003,         // 开锁记录上报
};

}

// ===== 应用 C：工业传感器 =====
namespace industrial_sensor_app {

/**
 * 工业传感器进程内消息类型
 *
 * 使用范围：30000-30999
 */
enum class SensorInprocMessageType : uint32_t {
    TEMPERATURE_ALERT = 30000,  // 温度越限告警
    HUMIDITY_ALERT = 30001,     // 湿度越限告警
    PRESSURE_ALERT = 30002,     // 压力越限告警
    CALIBRATION_DONE = 30003,   // 校准完成
};

/**
 * 工业传感器进程间消息类型
 *
 * 使用范围：31000-31999
 */
enum class SensorIpcMessageType : uint32_t {
    SENSOR_DATA = 31000,        // 传感器数据上报
    SENSOR_FAULT = 31001,       // 传感器故障告警
    CONFIG_UPDATE = 31002,      // 采集参数配置更新
    START_SAMPLING = 31003,     // 开始采样命令
    STOP_SAMPLING = 31004,      // 停止采样命令
};

}

}
