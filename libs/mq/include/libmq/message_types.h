/**
 * message_types.h
 * lib-mq 消息类型定义
 *
 * 本文件定义 lib-mq 库中使用的所有消息类型枚举、消息结构体和工具函数。
 *
 * 消息分类：
 * 1. 进程内消息（INPROC）：用于进程内部模块间通信，如拍照模块通知 MQTT 模块上传
 * 2. 进程间消息（IPC）：用于跨进程通信，如 iot_monitor -> iot_live 上报系统指标
 *
 * 消息类型范围分配：
 * - 0-9999:    库内置消息类型（由 lib-mq 定义，不可修改）
 * - 10000-65535: 用户自定义消息类型（各应用可在此范围内定义自己的类型）
 *
 * 自定义类型使用示例：
 *   // 在你的应用中定义自定义消息类型
 *   namespace my_app {
 *       enum class AppInprocMessageType : uint32_t {
 *           MOTION_DETECTED = 10000,
 *           FACE_RECOGNIZED = 10001,
 *       };
 *   }
 *
 *   // 使用时转换为 InprocMessageType
 *   manager->send(static_cast<InprocMessageType>(
 *       my_app::AppInprocMessageType::MOTION_DETECTED), payload);
 */

#ifndef LIBMQ_MESSAGE_TYPES_H
#define LIBMQ_MESSAGE_TYPES_H

#include <string>
#include <cstdint>
#include <chrono>

namespace libmq {

// ===== 用户自定义消息类型范围 =====

/**
 * 进程内消息类型 - 用户自定义范围起始值
 *
 * 应用可从此值开始定义自己的进程内消息类型，避免与库内置类型冲突。
 * 建议使用范围：10000-19999（每个应用分配 10000 个类型）
 *
 * 示例：
 *   enum class MyAppInprocMessageType : uint32_t {
 *       CUSTOM_EVENT_1 = 10000,
 *       CUSTOM_EVENT_2 = 10001,
 *   };
 */
constexpr uint32_t INPROC_USER_DEFINED_START = 10000;

/**
 * 进程间消息类型 - 用户自定义范围起始值
 *
 * 应用可从此值开始定义自己的进程间消息类型，避免与库内置类型冲突。
 * 建议使用范围：10000-19999（每个应用分配 10000 个类型）
 *
 * 示例：
 *   enum class MyAppIpcMessageType : uint32_t {
 *       CUSTOM_EVENT_1 = 10000,
 *       CUSTOM_EVENT_2 = 10001,
 *   };
 */
constexpr uint32_t IPC_USER_DEFINED_START = 10000;

/**
 * 消息域枚举
 *
 * 标识消息是通过进程内队列（INPROC）还是进程间队列（IPC）传输。
 * 用于 MessageHeader 中标记消息的来源和目标范围。
 */
enum class MessageDomain {
    INPROC,  // 进程内消息：同一进程内不同模块间的通信
    IPC      // 进程间消息：不同进程之间的通信（如 iot_live <-> ota_agent）
};

/**
 * 进程内消息类型枚举
 *
 * 用于进程内部模块间的消息传递，如摄像头模块通知 MQTT 模块上传照片。
 *
 * 消息范围分配：
 * - 0-9999:       库内置消息类型
 *   - 100-199:    照片相关
 *   - 200-299:    视频/媒体相关
 *   - 300-399:    系统/网络/配置相关
 *   - 400-499:    MQTT 连接状态相关
 * - 10000-65535:  用户自定义消息类型（应用层定义）
 *
 * 自定义类型使用示例：
 *   namespace my_app {
 *       enum class AppInprocType : uint32_t {
 *           MOTION_DETECTED = 10000,
 *       };
 *   }
 *   manager->send(static_cast<InprocMessageType>(my_app::AppInprocType::MOTION_DETECTED), payload);
 */
enum class InprocMessageType : uint32_t {
    NONE = 0,

    // ===== 照片相关（100-199） =====
    PHOTO_CAPTURED = 100,  // 照片拍摄完成，通知上传模块处理

    // ===== 视频/媒体相关（200-299） =====
    VIDEO_RECORD_STARTED = 200,   // 录像开始事件
    VIDEO_RECORD_STOPPED = 201,   // 录像停止事件
    CAMERA_DATA_COMPLETED = 202,  // 媒体文件完成（拍照或录像统一通知）

    // ===== 系统/网络/配置相关（300-399） =====
    SYSTEM_STATUS = 300,           // 系统状态更新
    NETWORK_STATUS = 301,          // 网络状态变化（上线/断网）
    DEVICE_CONFIG_READY = 302,     // 设备配置就绪通知（首次启动完成）
    DEVICE_CONFIG_UPDATED = 303,   // 设备配置更新通知（运行时远程配置变更）
    RPC_PUSH_CAMERA_STREAM = 304,  // RPC 命令：推送摄像头实时流
    RPC_PUSH_VIDEO_STREAM = 305,   // RPC 命令：推送录像回放流
    RPC_STOP_CAMERA_STREAM = 306,  // RPC 命令：停止摄像头实时流
    RPC_STOP_VIDEO_STREAM = 307,   // RPC 命令：停止录像回放流

    // ===== MQTT 连接状态相关（400-499） =====
    MQTT_CONNECTED = 400,        // MQTT 连接成功建立
    MQTT_DISCONNECTED = 401,     // MQTT 连接断开
    MQTT_MESSAGE_RECEIVED = 402  // 收到来自云端的 MQTT 消息（属性同步/RPC）
};

/**
 * 进程间消息类型枚举
 *
 * 用于不同进程之间的消息传递，如 iot_live <-> ota_agent <-> iot_monitor。
 *
 * 消息范围分配（避免不同模块消息类型冲突）：
 * - 0-9999:       库内置消息类型
 *   - 1000-1999:  心跳/健康检查
 *   - 2000-2999:  服务控制
 *   - 3000-3999:  配置相关
 *   - 4000-4999:  告警相关
 *   - 5000-5999:  OTA 相关（iot_live <-> ota_agent）
 * - 6000-6999:  监控相关（iot_live <-> iot_monitor）
 * - 12000-12099:  配置同步相关（通用，各服务共用）
 * - 12100-12199: 媒体服务相关（mediad <-> iot_agent）
 * - 10000-65535: 用户自定义消息类型（应用层定义）
 *
 * 自定义类型使用示例：
 *   namespace my_app {
 *       enum class AppIpcType : uint32_t {
 *           CUSTOM_ALARM = 10000,
 *       };
 *   }
 *   manager->send(static_cast<IpcMessageType>(my_app::AppIpcType::CUSTOM_ALARM), payload);
 */
enum class IpcMessageType : uint32_t {
    NONE = 0,

    // ===== 心跳/健康检查（1000-1999） =====
    HEARTBEAT = 1000,       // 心跳消息（定时发送，用于存活检测）
    HEARTBEAT_ACK = 1001,   // 心跳响应（收到心跳后回复）

    // ===== 服务控制（2000-2999） =====
    SERVICE_STATUS = 2000,   // 服务状态查询/上报
    SERVICE_RESTART = 2001,  // 服务重启命令（监控程序检测到异常后发送）
    SERVICE_STOP = 2002,     // 服务停止命令（优雅退出）

    // ===== 配置相关（3000-3999） =====
    CONFIG_UPDATE = 3000,    // 配置更新通知（云端下发新配置）
    CONFIG_REQUEST = 3001,   // 配置请求（子进程向主进程请求当前配置）

    // ===== 告警相关（4000-4999） =====
    ALARM_REPORT = 4000,  // 告警上报（设备异常事件通知云端）

    // ===== OTA 相关（5000-5999，iot_live <-> ota_agent） =====
    OTA_REQUEST = 5000,  // iot_live -> ota_agent：下发 OTA 升级任务（fw_/sw_ 两种类型）
    OTA_STATUS = 5001,   // ota_agent -> iot_live：回传 OTA 升级进度（由 iot_live 转发到云端）

    // ===== 监控相关（6000-6999，iot_live <-> iot_monitor） =====
    MONITOR_TELEMETRY = 6000,  // iot_monitor -> iot_live：系统指标遥测数据（CPU/内存/磁盘等）
    MONITOR_ALERT = 6001,      // iot_monitor -> iot_live：系统告警（崩溃循环/磁盘满/内存不足等）

    // ===== 配置同步（12000-12099，通用，各服务共用） =====
    CONFIG_SYNC_STARTED   = 12000,  // 服务启动完成 {"serviceName":...,"configVersion":...}
    CONFIG_SYNC_UPDATE    = 12001,  // 配置更新 {"version":...,"config":{...}}
    CONFIG_SYNC_ACK       = 12002,  // 配置确认 {"version":...,"success":true|false,"error":"..."}
    CONFIG_SYNC_REQUEST   = 12003,  // 服务请求当前配置 {"serviceName":...}（服务 -> iot_agent）
    CONFIG_SYNC_RESPONSE  = 12004,  // 返回当前配置 {"version":...,"config":{...}}（iot_agent -> 服务）

    // ===== 媒体服务事件（12100-12199，mediad -> iot_agent） =====
    MEDIA_FILE_READY        = 12100,  // 媒体文件就绪 {"triggerType":...,"path":...,"size":...}
    MEDIA_STATE_CHANGED     = 12101,  // 状态变化 {"camera":"running|stopped",...}
    MEDIA_CONFIG_APPLIED    = 12102,  // 配置已生效 {"restarted":true|false}
    MEDIA_STARTED           = 12103,  // mediad 启动完成，所有服务初始化完毕
    MEDIA_HEARTBEAT_REQUEST = 12104,  // iot_agent 请求心跳（iot_agent 重启后广播）
    MEDIA_HEARTBEAT_ACK     = 12105,  // mediad 心跳确认（收到 REQUEST 后发送）
};

/**
 * 基础消息头结构体
 *
 * 每条消息都包含此消息头，用于标识消息类型、来源、目标和时间顺序。
 * 消息头后紧跟 JSON 格式的消息负载。
 */
struct MessageHeader {
    MessageDomain domain;       // 消息域（INPROC 或 IPC），标识消息传输通道
    uint32_t type;              // 消息类型（对应 InprocMessageType 或 IpcMessageType 枚举值）
    uint64_t timestamp;         // 消息创建时间戳（毫秒），用于消息排序和超时检测
    uint32_t sequence;          // 消息序列号（单调递增），用于消息追踪和调试
    uint32_t payloadSize;       // 消息负载大小（字节），用于解析时预分配缓冲区

    // 路由字段（IPC 多对多模式用）
    // 用固定大小 char 数组，保证 memcpy 序列化安全（跨进程传输 std::string 指针会崩溃）
    static constexpr size_t kNameMaxLen = 32;
    char from[kNameMaxLen];     // 发送方名字，如 "mediad"
    char to[kNameMaxLen];       // 接收方名字，如 "iot_agent"；空=发给中心节点处理

    /**
     * 默认构造函数
     *
     * 初始化为默认值：INPROC 域，类型 0，时间戳和序列号为 0，路由字段清零。
     */
    MessageHeader()
        : domain(MessageDomain::INPROC)
        , type(0)
        , timestamp(0)
        , sequence(0)
        , payloadSize(0)
        , from{}
        , to{} {}
};

/**
 * 照片拍摄消息结构体
 *
 * 当摄像头模块完成拍照后，通过此结构体将照片信息传递给上传模块。
 * 序列化为 JSON 后通过 INPROC 队列发送。
 */
struct PhotoCapturedMessage {
    std::string photoName;      // 照片文件名（如 "20260528_103000.jpg"）
    std::string photoPath;      // 照片完整路径（如 "/mnt/storage/photos/20260528_103000.jpg"）
    uint64_t fileSize;          // 照片文件大小（字节）
    uint64_t captureTime;       // 拍摄时间戳（毫秒），用于云端展示
    uint32_t width;             // 图片宽度（像素）
    uint32_t height;            // 图片高度（像素）

    /**
     * 默认构造函数
     *
     * 初始化数值字段为 0，字符串字段为空。
     */
    PhotoCapturedMessage()
        : fileSize(0)
        , captureTime(0)
        , width(0)
        , height(0) {}
};

/**
 * 心跳消息结构体
 *
 * 用于进程间健康检查，定时发送以表明进程存活。
 * 包含进程资源使用情况，便于监控服务判断是否异常。
 */
struct HeartbeatMessage {
    uint32_t processId;         // 进程 ID（PID），用于唯一标识发送方进程
    std::string processName;    // 进程名称（如 "ota_agent"、"iot_monitor"）
    uint64_t uptime;            // 进程运行时长（秒），从启动开始计算
    uint32_t cpuUsage;          // CPU 使用率（百分比，0-100），用于检测 CPU 异常
    uint32_t memoryUsage;       // 内存使用率（百分比，0-100），用于检测内存泄漏

    /**
     * 默认构造函数
     *
     * 初始化所有字段为 0 或空字符串。
     */
    HeartbeatMessage()
        : processId(0)
        , uptime(0)
        , cpuUsage(0)
        , memoryUsage(0) {}
};

/**
 * 服务状态消息结构体
 *
 * 用于进程间服务状态查询和上报。
 * 监控服务可通过此消息了解各子服务的运行状态。
 */
struct ServiceStatusMessage {
    uint32_t processId;         // 进程 ID（PID）
    std::string serviceName;    // 服务名称（如 "camera_service"、"mqtt_service"）
    uint32_t status;            // 服务状态码：0=已停止，1=运行中，2=异常
    uint64_t lastUpdateTime;    // 最后状态更新时间戳（毫秒）
    std::string errorMessage;   // 错误信息（status=2 时提供详细错误原因）

    /**
     * 默认构造函数
     *
     * 初始化状态为"已停止"（status=0），其他字段为 0 或空。
     */
    ServiceStatusMessage()
        : processId(0)
        , status(0)
        , lastUpdateTime(0) {}
};

/**
 * 获取当前时间戳（毫秒）
 *
 * 使用系统时钟获取从 Unix 纪元（1970-01-01 00:00:00 UTC）到现在的毫秒数。
 * 用于消息头中的 timestamp 字段填充。
 *
 * @return 当前时间戳（毫秒），可用于 MessageHeader::timestamp
 */
inline uint64_t getCurrentTimestamp() {
    // 获取当前系统时间点
    auto now = std::chrono::system_clock::now();
    // 计算从纪元到现在的持续时间
    auto duration = now.time_since_epoch();
    // 转换为毫秒精度并返回
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

} // namespace libmq

#endif // LIBMQ_MESSAGE_TYPES_H
