/**
 * @file ipc_event_handler.cpp
 * @brief IPC 事件处理器实现 —— 订阅 mediad 事件并上报云端
 */
#include "ipc_event_handler.h"

#include "cloud/cloud_service.h"
#include "libmq/message_types.h"
#include "nc/common/log_utils.h"
#include "upload/file_upload_service.h"

#include "cJSON.h"

namespace iot_agent {

void IpcEventHandler::subscribe(libmq::MessageManager& manager) {
    /* MEDIA_FILE_READY = 12100：拍照/录像文件已生成 → 上报 Telemetry */
    manager.subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_FILE_READY),
        [this](const libmq::MessageHeader& h, const std::string& payload) {
            onMediaFileReady(h, payload);
        });

    /* MEDIA_STATE_CHANGED = 12101：状态变化 → 上报 Attributes */
    manager.subscribeIpc(
        static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_STATE_CHANGED),
        [this](const libmq::MessageHeader& h, const std::string& payload) {
            onMediaStateChanged(h, payload);
        });
}

void IpcEventHandler::onMediaFileReady(const libmq::MessageHeader& h, const std::string& payload) {
    NC_LOGI("[IPC] MEDIA_FILE_READY from={}: {}", h.from, payload.c_str());
    /* mediad 发来的是文件通知，iot_agent 包装成云端事件格式：
     * {"event_type":"media_file_ready","event_source":"mediad","event_time":...,
     *  "trigger_type":"timer","file_type":"image",
     *  "file_name":"xxx.jpg","file_path":"/mnt/...","file_size":3611,
     *  "start_time":...,"duration":...}（后两个仅录像有，回放选文件用） */

    cJSON* notify = cJSON_Parse(payload.c_str());
    if (!notify) return;

    /* ---- 触发文件上传：解析字段 → 入队任务（非阻塞，失败只记日志） ---- */
    if (m_uploadService) {
        enqueueUploadTasks(notify);
    }

    if (!m_cloudService) {
        cJSON_Delete(notify);
        return;
    }

    cJSON* event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "event_type", "media_file_ready");
    cJSON_AddStringToObject(event, "event_source", h.from);

    /* 把 mediad 里的 timestamp 改成 event_time */
    cJSON* tsItem = cJSON_GetObjectItemCaseSensitive(notify, "timestamp");
    if (cJSON_IsNumber(tsItem)) {
        cJSON_AddNumberToObject(event, "event_time", tsItem->valuedouble);
    }

    /* 复制 mediad 发来的其他字段（缩略图三字段 + start_time/duration 一并转发给云端） */
    const char* copyKeys[] = {"trigger_type", "file_type", "file_name", "file_path",
                              "file_size", "thumb_name", "thumb_path", "thumb_size",
                              "start_time", "duration"};
    for (const char* key : copyKeys) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(notify, key);
        if (item) {
            cJSON_AddItemReferenceToObject(event, key, item);
        }
    }

    char* printed = cJSON_PrintUnformatted(event);
    if (printed) {
        /* 遥测上报：主题由 CloudService 问平台适配器要，这里不写死 */
        m_cloudService->publishTelemetry(printed);
        cJSON_free(printed);
    }
    cJSON_Delete(event);
    cJSON_Delete(notify);
}

void IpcEventHandler::onMediaStateChanged(const libmq::MessageHeader& h, const std::string& payload) {
    NC_LOGI("[IPC] MEDIA_STATE_CHANGED from={}: {}", h.from, payload.c_str());
    /* 上报到云端 Attributes（设备状态） */
    if (m_cloudService) {
        /* 包装成 {"media_state":{...}}，主题由 CloudService 决定 */
        std::string attributes = "{\"media_state\":" + payload + "}";
        m_cloudService->publishAttributes(attributes);
    }
}

/**
 * 从 MEDIA_FILE_READY 载荷解析文件信息并入队上传任务
 *
 * 规则（产品决策：只传缩略图，源文件不上传，省流量）：
 *   - image / video：只入队缩略图（thumb_path），源文件保留在设备本地不上传
 *   - multipart 分片链路保留在 FileUploadService 中，未来如需上传源文件，
 *     在此处入队源文件任务即可
 *   - 入队失败（服务未运行/队列满）只记日志，不影响事件上报链路
 */
void IpcEventHandler::enqueueUploadTasks(cJSON* notify) {
    const cJSON* typeItem = cJSON_GetObjectItemCaseSensitive(notify, "file_type");
    if (!cJSON_IsString(typeItem)) {
        NC_LOGW("[IPC] MEDIA_FILE_READY 字段缺失，跳过上传");
        return;
    }

    const std::string fileType = typeItem->valuestring;

    if (fileType == "image" || fileType == "video") {
        /* 只传缩略图：源文件（原图/视频）保留在设备本地不上传 */
        const cJSON* thumbPath = cJSON_GetObjectItemCaseSensitive(notify, "thumb_path");
        const cJSON* thumbSize = cJSON_GetObjectItemCaseSensitive(notify, "thumb_size");
        if (!cJSON_IsString(thumbPath) || thumbPath->valuestring[0] == '\0') {
            NC_LOGW("[IPC] 缩略图路径缺失，跳过上传");
            return;
        }
        const int64_t thumbBytes = cJSON_IsNumber(thumbSize)
                                       ? static_cast<int64_t>(thumbSize->valuedouble)
                                       : 0;
        const std::string thumbPathStr = thumbPath->valuestring;
        /* 缩略图文件名约定为 源文件名_thumb.jpg，若取不到则从路径提取 */
        const size_t slash = thumbPathStr.find_last_of('/');
        const std::string thumbName = (slash != std::string::npos)
                                          ? thumbPathStr.substr(slash + 1)
                                          : thumbPathStr;
        enqueueOne(thumbPathStr, thumbName, "thumbnail", thumbBytes);
    } else {
        /* 未知类型：当前只传缩略图策略，源文件不上传 */
        NC_LOGI("[IPC] file_type={} 按策略不上传（只传缩略图）", fileType.c_str());
    }
}

void IpcEventHandler::enqueueOne(const std::string& path, const std::string& name,
                                 const std::string& type, int64_t size) {
    UploadTask task;
    task.localPath = path;
    task.fileName = name;
    task.fileType = type;
    task.fileSize = size;
    if (!m_uploadService->enqueue(task)) {
        NC_LOGW("[IPC] 上传任务入队失败: {}", path.c_str());
    }
}

} // namespace iot_agent
