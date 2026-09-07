/**
 * @file rpc_handler.cpp
 * @brief RPC 指令处理实现
 *
 * 处理流程：
 *   1) 打日志，记录收到了什么 RPC 指令
 *   2) 按 method 分发到具体处理函数
 *   3) 回复 TB 执行结果（成功/失败）
 *
 * 支持的指令：
 *   - uploadFile：按需上传设备上的原始文件（用户想查看原图/原视频时由云端下发）
 *   - startLiveStream：开始实时流推送（通过 IPC 发命令给 mediad）
 *   - stopLiveStream：停止实时流推送（通过 IPC 发命令给 mediad）
 *   - restart：重启设备（先回复云端，1 秒后执行 reboot）
 *   - reset：恢复出厂配置（清掉 device_config.json + token.json，回复后自动重启）
 */
#include "router/rpc_handler.h"

#include "config/agent_config.h"
#include "upload/file_upload_service.h"
#include "nc/common/log_utils.h"

#include "cJSON.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>

/* mediad 命令编号（与 mediad/MediadMessages.h 的 Command 枚举保持一致）
 * 不直接 include 头文件，避免 iot_agent 依赖 mediad 的头文件路径 */
namespace {
constexpr uint32_t kMediaStreamStart = 12010;
constexpr uint32_t kMediaStreamStop  = 12011;
} // namespace

namespace iot_agent {

RpcHandler::RpcHandler() = default;
RpcHandler::~RpcHandler() = default;

void RpcHandler::setPublishCallback(PublishCallback cb) {
    m_publish = std::move(cb);
}

void RpcHandler::setIpcCommandCallback(IpcCommandCallback cb) {
    m_ipcCommand = std::move(cb);
}

void RpcHandler::setUploadService(FileUploadService* service) {
    m_uploadService = service;
}

void RpcHandler::setConfig(AgentConfig* config) {
    m_config = config;
}

/* ====================================================================
 * handleRpc：处理一条 RPC 指令（按 method 分发）
 * ==================================================================== */
void RpcHandler::handleRpc(const RpcRequest& req) {
    NC_LOGI("[RpcHandler] 收到 RPC: method={} id={} params={} (from {})",
             req.method.c_str(), req.requestId.c_str(),
             req.paramsJson.c_str(), req.source.c_str());

    if (req.method == "uploadFile") {
        handleUploadFile(req);
        return;
    }

    if (req.method == "startLiveStream") {
        handleStartLiveStream(req);
        return;
    }

    if (req.method == "stopLiveStream") {
        handleStopLiveStream(req);
        return;
    }

    if (req.method == "restart") {
        handleRestart(req);
        return;
    }

    if (req.method == "reset") {
        handleReset(req);
        return;
    }

    /* 未实现的指令：记日志并回复不支持，避免云端一直等响应 */
    NC_LOGW("[RpcHandler] 不支持的 RPC 指令: method={}", req.method.c_str());
    sendResponse(req.requestId,
                 std::string("{\"success\":false,\"error\":\"unsupported method: ") +
                 req.method + "\"}");
}

/* ====================================================================
 * handleUploadFile：按需上传原始文件
 *
 * 参数（paramsJson）：
 *   {
 *     "fileType": "image" | "video",   文件类型（决定直传/分片上传）
 *     "filePath": "/mnt/emmc/...",     文件在设备上的完整路径
 *     "fileId":   "xxx"                文件唯一 ID（云端用来关联是哪个文件的请求，
 *                                      会随 media_file_upload_result 事件原样回传）
 *   }
 *
 * 响应只表示"任务是否入队成功"，实际上传进度/结果由
 * media_file_upload_result 事件上报（带 file_id 关联，成功失败都发，靠 ok 字段区分）。
 * ==================================================================== */
void RpcHandler::handleUploadFile(const RpcRequest& req) {
    /* 上传服务没接上，直接报错（main.cpp 漏配的情况） */
    if (!m_uploadService) {
        NC_LOGE("[RpcHandler] uploadFile 失败：上传服务未设置");
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"upload service not ready\"}");
        return;
    }

    /* 解析参数 */
    cJSON* params = cJSON_Parse(req.paramsJson.c_str());
    if (!params) {
        NC_LOGW("[RpcHandler] uploadFile 参数不是合法 JSON: {}", req.paramsJson.c_str());
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"invalid params json\"}");
        return;
    }

    const cJSON* typeItem = cJSON_GetObjectItemCaseSensitive(params, "fileType");
    const cJSON* pathItem = cJSON_GetObjectItemCaseSensitive(params, "filePath");
    const cJSON* idItem   = cJSON_GetObjectItemCaseSensitive(params, "fileId");

    /* 三个参数都必填：类型决定上传方式，路径是文件位置，ID 用于结果关联 */
    if (!cJSON_IsString(typeItem) || !cJSON_IsString(pathItem) || !cJSON_IsString(idItem) ||
        pathItem->valuestring[0] == '\0' || idItem->valuestring[0] == '\0') {
        NC_LOGW("[RpcHandler] uploadFile 参数缺失（fileType/filePath/fileId 必填）");
        cJSON_Delete(params);
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"missing fileType/filePath/fileId\"}");
        return;
    }

    const std::string fileType = typeItem->valuestring;
    const std::string filePath = pathItem->valuestring;
    const std::string fileId   = idItem->valuestring;
    cJSON_Delete(params);

    /* 只认 image / video 两种类型，其他类型按错误处理 */
    if (fileType != "image" && fileType != "video") {
        NC_LOGW("[RpcHandler] uploadFile 不支持的文件类型: {}", fileType.c_str());
        sendResponse(req.requestId,
                     std::string("{\"success\":false,\"error\":\"unsupported fileType: ") +
                     fileType + "\"}");
        return;
    }

    /* 文件先确认存在：录像可能被循环清理掉了，提前报错比让用户干等强 */
    struct stat st;
    if (::stat(filePath.c_str(), &st) != 0 || st.st_size <= 0) {
        NC_LOGW("[RpcHandler] uploadFile 文件不存在或为空: {}", filePath.c_str());
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"file not found on device\"}");
        return;
    }

    /* 组装上传任务入队：fileName 从路径里取最后一段 */
    UploadTask task;
    task.localPath = filePath;
    task.fileName  = filePath.substr(filePath.find_last_of('/') + 1);
    task.fileType  = fileType;
    task.fileSize  = static_cast<int64_t>(st.st_size);
    task.fileId    = fileId;

    if (!m_uploadService->enqueue(task)) {
        NC_LOGW("[RpcHandler] uploadFile 入队失败: {}", filePath.c_str());
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"enqueue failed (service not running or queue full)\"}");
        return;
    }

    NC_LOGI("[RpcHandler] uploadFile 已入队: fileId={} type={} path={} size={}",
            fileId.c_str(), fileType.c_str(), filePath.c_str(),
            static_cast<long long>(st.st_size));

    /* 回复云端：任务已受理，结果看 media_file_upload_result 事件 */
    std::string resp = "{\"success\":true,\"fileId\":\"" + fileId + "\"}";
    sendResponse(req.requestId, resp);
}

/* ====================================================================
 * handleStartLiveStream：开始实时流推送
 *
 * 通过 IPC 发 MEDIA_STREAM_START 命令给 mediad，
 * mediad 的 LiveStreamService 进入 Relay 模式（起 SRS WHIP 推流）。
 * 这是 fire-and-forget：命令发出就回复成功，
 * 实际推流状态由 MEDIA_STATE_CHANGED 事件上报。
 *
 * RPC 参数可携带 accessToken（可选字段）：
 *   {"accessToken": "xxx"}
 * accessToken: 平台下发的认证令牌，拼进 WHIP URL
 *
 * 注意：P2P 模式不走此 RPC，由观看端通过 MQTT 信令直接发 offer 触发
 * ==================================================================== */
void RpcHandler::handleStartLiveStream(const RpcRequest& req) {
    if (!m_ipcCommand) {
        NC_LOGE("[RpcHandler] startLiveStream 失败：IPC 命令回调未设置");
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"ipc command channel not available\"}");
        return;
    }

    /* 从 params 提取 accessToken（可选字段），拼进 IPC payload */
    std::string ipcPayload = "{}";
    if (!req.paramsJson.empty()) {
        cJSON* params = cJSON_Parse(req.paramsJson.c_str());
        if (params) {
            const cJSON* tokenItem = cJSON_GetObjectItemCaseSensitive(params, "accessToken");
            if (cJSON_IsString(tokenItem) && tokenItem->valuestring) {
                cJSON* payload = cJSON_CreateObject();
                cJSON_AddStringToObject(payload, "accessToken", tokenItem->valuestring);
                char* printed = cJSON_PrintUnformatted(payload);
                if (printed) {
                    ipcPayload = printed;
                    cJSON_free(printed);
                }
                cJSON_Delete(payload);
            }
            cJSON_Delete(params);
        }
    }

    bool sent = m_ipcCommand(kMediaStreamStart, ipcPayload);
    if (!sent) {
        NC_LOGW("[RpcHandler] startLiveStream IPC 命令发送失败（mediad 可能不在线）");
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"ipc send failed (mediad offline?)\"}");
        return;
    }

    NC_LOGI("[RpcHandler] startLiveStream IPC 命令已发送 (relay mode)");
    sendResponse(req.requestId, "{\"success\":true}");
}

/* ====================================================================
 * handleStopLiveStream：停止实时流推送
 *
 * 通过 IPC 发 MEDIA_STREAM_STOP 命令给 mediad，
 * mediad 的 LiveStreamService 停止手动推流。
 * RPC 参数可携带 accessToken（可选字段）：
 *   {"accessToken": "xxx"}
 * accessToken: 必须与 startLiveStream 时一致，否则 mediad 会拒绝停止
 * ==================================================================== */
void RpcHandler::handleStopLiveStream(const RpcRequest& req) {
    if (!m_ipcCommand) {
        NC_LOGE("[RpcHandler] stopLiveStream 失败：IPC 命令回调未设置");
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"ipc command channel not available\"}");
        return;
    }

    /* 从 params 提取 accessToken（可选字段），拼进 IPC payload */
    std::string ipcPayload = "{}";
    if (!req.paramsJson.empty()) {
        cJSON* params = cJSON_Parse(req.paramsJson.c_str());
        if (params) {
            const cJSON* tokenItem = cJSON_GetObjectItemCaseSensitive(params, "accessToken");
            if (cJSON_IsString(tokenItem) && tokenItem->valuestring) {
                cJSON* payload = cJSON_CreateObject();
                cJSON_AddStringToObject(payload, "accessToken", tokenItem->valuestring);
                char* jsonStr = cJSON_PrintUnformatted(payload);
                if (jsonStr) {
                    ipcPayload = jsonStr;
                    free(jsonStr);
                }
                cJSON_Delete(payload);
            }
            cJSON_Delete(params);
        }
    }

    bool sent = m_ipcCommand(kMediaStreamStop, ipcPayload);
    if (!sent) {
        NC_LOGW("[RpcHandler] stopLiveStream IPC 命令发送失败（mediad 可能不在线）");
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"ipc send failed (mediad offline?)\"}");
        return;
    }

    NC_LOGI("[RpcHandler] stopLiveStream IPC 命令已发送");
    sendResponse(req.requestId, "{\"success\":true}");
}

/* ====================================================================
 * handleRestart：重启设备
 *
 * 先回复云端成功，然后 fork 子进程等 1 秒后执行 reboot。
 * 1 秒等待是给 MQTT 响应发出去，避免回复还没到云端设备就重启了。
 * ==================================================================== */
void RpcHandler::handleRestart(const RpcRequest& req) {
    NC_LOGI("[RpcHandler] 收到 restart 指令，准备重启设备");
    sendResponse(req.requestId, "{\"success\":true}");

    /* fork 子进程做延迟重启，不阻塞当前回复流程 */
    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：等 1 秒让 MQTT 响应发出去，再重启 */
        sleep(1);
        NC_LOGI("[RpcHandler] 执行 reboot");
        system("reboot");
        _exit(0);
    } else if (pid < 0) {
        NC_LOGE("[RpcHandler] fork 失败，无法重启");
    }
}

/* ====================================================================
 * handleReset：恢复出厂配置
 *
 * 通过 AgentConfig 删掉 device_config.json + token.json，
 * 回复云端成功后自动重启设备。
 * ==================================================================== */
void RpcHandler::handleReset(const RpcRequest& req) {
    if (!m_config) {
        NC_LOGE("[RpcHandler] reset 失败：配置未设置");
        sendResponse(req.requestId,
                     "{\"success\":false,\"error\":\"config not configured\"}");
        return;
    }

    NC_LOGI("[RpcHandler] 收到 reset 指令，开始清理配置文件");

    m_config->reset();

    NC_LOGI("[RpcHandler] 配置文件已清理，准备重启设备");
    sendResponse(req.requestId, "{\"success\":true}");

    /* 回复发出去后重启设备（和 restart 同样的延迟逻辑） */
    pid_t pid = fork();
    if (pid == 0) {
        sleep(1);
        NC_LOGI("[RpcHandler] reset 完成，执行 reboot");
        system("reboot");
        _exit(0);
    } else if (pid < 0) {
        NC_LOGE("[RpcHandler] fork 失败，无法重启");
    }
}

/* ====================================================================
 * sendResponse：回复 TB RPC 结果
 *
 * TB 的回复主题：v1/devices/me/rpc/response/{requestId}
 * ==================================================================== */
void RpcHandler::sendResponse(const std::string& requestId, const std::string& resultJson) {
    if (!m_publish) {
        NC_LOGW("[RpcHandler] publish 回调未设置，无法回复 RPC id={}", requestId.c_str());
        return;
    }

    std::string topic = "v1/devices/me/rpc/response/" + requestId;
    bool ok = m_publish(topic, resultJson);
    if (ok) {
        NC_LOGI("[RpcHandler] RPC 响应已发送: id={} topic={}", requestId.c_str(), topic.c_str());
    } else {
        NC_LOGW("[RpcHandler] RPC 响应发送失败: id={} topic={}", requestId.c_str(), topic.c_str());
    }
}

} // namespace iot_agent
