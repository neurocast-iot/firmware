/**
 * @file ipc_event_handler.h
 * @brief IPC 事件处理器 —— 订阅并处理 mediad 上报的媒体事件
 *
 * 职责：
 *   - 订阅 MEDIA_FILE_READY（拍照/录像文件就绪）→ 包装成云端事件 → 上报 Telemetry
 *   - 订阅 MEDIA_STATE_CHANGED（状态变化）→ 上报 Attributes
 *
 * 事件上报规范：
 *   - mediad 只管"文件生成好了"，发简单的文件信息
 *   - iot_agent 负责把文件信息包装成"云端事件"，添加事件通用字段
 *
 * 为什么单独封装：
 *   - main.cpp 不应该关心具体事件的处理逻辑
 *   - 事件处理逻辑可以单独测试
 *   - 新增事件类型时不用改 main
 */

/**
 * IPC event handler — subscribes to and processes media events reported by mediad
 *
 * Responsibilities:
 *   - Subscribe to MEDIA_FILE_READY (snapshot/recording file ready) -> wrap as cloud event -> report Telemetry
 *   - Subscribe to MEDIA_STATE_CHANGED (state change) -> report Attributes
 *
 * Event reporting contract:
 *   - mediad only reports "file is ready" with basic file info
 *   - iot_agent wraps the file info into a "cloud event" with common event fields
 *
 * Why a separate wrapper:
 *   - main.cpp should not care about specific event handling logic
 *   - Event handling logic can be tested independently
 *   - Adding new event types doesn't require changes to main
 */
#pragma once

#include "libmq/message_manager.h"

#include <string>

namespace iot_agent {

class CloudService;
class FileUploadService;
}

/* 前向声明 cJSON（C 库结构体），头文件不引入 cJSON.h，缩短编译链 */
struct cJSON;

namespace iot_agent {

/**
 * @brief IPC 事件处理器
 *
 * 使用方式：
 *   IpcEventHandler handler;
 *   handler.setCloudService(&cloudService);
 *   handler.subscribe(*ipcHub.manager());
 *   // 之后 ipcHub 收到事件会自动上报到云端
 */
class IpcEventHandler {
public:
    IpcEventHandler() = default;

    /**
     * @brief 设置 CloudService，用于上报事件到云端
     *
     * 不设置的话，只会打印日志，不会上报
     */
    void setCloudService(CloudService* service) { m_cloudService = service; }

    /**
     * @brief 设置文件上传服务，MEDIA_FILE_READY 事件触发时入队上传任务
     *
     * 不设置的话，只上报云端事件，不上传文件
     */
    void setUploadService(FileUploadService* service) { m_uploadService = service; }

    /**
     * @brief 订阅所有事件到指定的 MessageManager
     *
     * 调用后，MessageManager 收到相关事件会自动上报到云端
     */
    void subscribe(libmq::MessageManager& manager);

private:
    /** 处理 MEDIA_FILE_READY：入队上传 + 包装成云端事件上报遥测 */
    void onMediaFileReady(const libmq::MessageHeader& h, const std::string& payload);

    /** 处理 MEDIA_STATE_CHANGED：上报到云端属性 */
    void onMediaStateChanged(const libmq::MessageHeader& h, const std::string& payload);

    /** 从 MEDIA_FILE_READY 载荷解析文件信息并入队上传任务 */
    void enqueueUploadTasks(cJSON* notifyJson);

    /** 单个任务入队（失败仅记日志） */
    void enqueueOne(const std::string& path, const std::string& name,
                    const std::string& type, int64_t size);

    CloudService* m_cloudService = nullptr;
    FileUploadService* m_uploadService = nullptr;
};

} // namespace iot_agent
