/**
 * @file CommandRouter.cpp
 * @brief 命令路由器实现
 *
 * mediad 的 IPC 命令总入口：请求/应答都是 JSON 字符串。
 * 请求格式 {"type": 命令号, "payload": 参数对象}，
 * 应答格式 {"code": 0, "message": "ok", "data": 可选结果}。
 * 所有 handle 函数都在 IPC 命令线程上串行执行，不会同时跑两个。
 */
#include "ipc/CommandRouter.h"
#include "ipc/MediadMessages.h"

#include "config/ConfigStore.h"
#include "services/CameraService.h"
#include "services/OsdService.h"
#include "services/RecordService.h"
#include "services/LiveStreamService.h"
#include "triggers/trigger_manager.h"
#include "triggers/trigger.h"

#include "nc/common/log_utils.h"
#include "libmq/message_types.h"

#include "cJSON.h"

namespace mediad {

/**
 * 解析请求并分发到对应的 handle 函数
 *
 * 不管输入多乱（非 JSON、缺 type、未知命令）都保证返回一条带
 * 错误码的应答，不抛异常不返空串：对端（mediactl/iot_live）
 * 是阻塞等应答的，不回包会让对方干等到超时。
 */
std::string CommandRouter::dispatch(const std::string& request) {
    cJSON* root = cJSON_Parse(request.c_str());
    if (!root) {
        return makeResponse(RespCode::BadRequest, "invalid json envelope");
    }

    const cJSON* typeItem = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsNumber(typeItem)) {
        cJSON_Delete(root);
        return makeResponse(RespCode::BadRequest, "missing type field");
    }
    const uint32_t type = static_cast<uint32_t>(typeItem->valueint);
    /* payload 允许缺失（得到 nullptr）：无参命令如 RECORD_STOP 不带它，
     * 各 handle 函数自己用 cJSON_IsObject 判断后再取字段 */
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");

    NC_LOGD("CommandRouter: dispatch type={}", type);

    std::string response;
    switch (static_cast<Command>(type)) {
            case Command::MEDIA_SNAPSHOT:
                response = handleSnapshot(payload);
                break;
            case Command::MEDIA_GET_STATUS:
                response = handleGetStatus();
                break;
            case Command::MEDIA_GET_CONFIG:
                response = handleGetConfig();
                break;
            case Command::MEDIA_STREAM_START:
                response = handleStreamStart(payload);
                break;
            case Command::MEDIA_STREAM_STOP:
                response = handleStreamStop(payload);
                break;
            default:
                response = makeResponse(RespCode::BadRequest,
                                        "unknown command type " + std::to_string(type));
                break;
        }

    cJSON_Delete(root);
    return response;
}

/**
 * 立即拍一张照片，成功时 data.path 返回文件路径
 *
 * 走 TriggerManager 通路：跟定时/蓝牙拍一样的代码路径，
 * 拍完自动调 fileReadyCallback 通知 iot_agent 上传。
 *
 * 阻塞式：拍完（含 JPEG 编码落盘，实测百毫秒级）才回应答，
 * 调用方拿到 path 即可直接读文件。相机未运行时返回
 * CameraNotReady，让调用方能区分“环境没就绪”和“拍照失败”。
 */
std::string CommandRouter::handleSnapshot(const cJSON* /*payload*/) {
    /* 构造一个高优先级的 Snapshot 事件，走 trigger 系统 */
    TriggerEvent event;
    event.action = ActionType::Snapshot;
    event.priority = 100;   /* 手动拍优先级高于定时/蓝牙 */
    event.burstCount = 1;
    event.burstIntervalMs = 0;
    std::strncpy(event.sourceId, "manual", sizeof(event.sourceId) - 1);
    event.sourceId[sizeof(event.sourceId) - 1] = '\0';
    std::strncpy(event.prefix, "manual_", sizeof(event.prefix) - 1);
    event.prefix[sizeof(event.prefix) - 1] = '\0';

    SnapshotResult result = m_triggerManager.fireEvent(event);
    if (!result.ok) {
        RespCode code = (result.errMsg == "camera not running") ? RespCode::CameraNotReady
                                                                : RespCode::Internal;
        return makeResponse(code, result.errMsg);
    }

    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", result.path.c_str());
    char* printed = cJSON_PrintUnformatted(data);
    std::string dataJson = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(data);
    return makeResponse(RespCode::Ok, "ok", dataJson);
}

/* 录像控制已统一由 triggers 系统管理，不再提供 IPC 命令入口 */

/* RPC 调用：固定走 SFU 模式，直接起 WHIP 推流
 * payload 可携带 accessToken（平台下发的认证令牌） */
std::string CommandRouter::handleStreamStart(const cJSON* payload) {
    std::string accessToken;
    /* 从 payload 提取 accessToken（可选字段） */
    if (cJSON_IsObject(payload)) {
        const cJSON* tokenItem = cJSON_GetObjectItemCaseSensitive(payload, "accessToken");
        if (cJSON_IsString(tokenItem) && tokenItem->valuestring) {
            accessToken = tokenItem->valuestring;
        }
    }
    auto err = m_live.startManualPush(accessToken);
    if (err != LiveStreamService::ManualPushError::Ok) {
        RespCode code = RespCode::Internal;
        switch (err) {
            case LiveStreamService::ManualPushError::Disabled:
                code = RespCode::BadRequest; break;
            case LiveStreamService::ManualPushError::Busy:
                code = RespCode::Busy; break;
            default: break;
        }
        return makeResponse(code, LiveStreamService::toManualPushErrorMsg(err));
    }
    return makeResponse(RespCode::Ok, "ok");
}

/* 停手动推流；只能停 handleStreamStart 起的推流，
 * 观看者触发的自动推流由 LiveStreamService 自己按人数回收
 * payload 可携带 accessToken，只有 token 一致才允许停止 */
std::string CommandRouter::handleStreamStop(const cJSON* payload) {
    std::string accessToken;
    if (cJSON_IsObject(payload)) {
        const cJSON* tokenItem = cJSON_GetObjectItemCaseSensitive(payload, "accessToken");
        if (cJSON_IsString(tokenItem) && tokenItem->valuestring) {
            accessToken = tokenItem->valuestring;
        }
    }
    auto err = m_live.stopManualPush(accessToken);
    if (err != LiveStreamService::ManualPushError::Ok) {
        RespCode code = (err == LiveStreamService::ManualPushError::TokenMismatch)
                            ? RespCode::BadRequest
                            : RespCode::Internal;
        return makeResponse(code, LiveStreamService::toManualPushErrorMsg(err));
    }
    return makeResponse(RespCode::Ok, "ok");
}

/**
 * 汇总各服务的运行状态，供 mediactl status 一眼看清设备在干什么
 *
 * 只读各服务已有的状态接口，不碰相机 SDK，随时调都安全。
 * snapshot_scheduled/record_scheduled 取自配置而非线程实况：
 * 定时开关的意图就存在配置里，配置说开就是开。
 */
std::string CommandRouter::handleGetStatus() {
    cJSON* data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "camera",
                            m_camera.isRunning() ? "running" : "stopped");
    cJSON_AddBoolToObject(data, "osd_active", m_osd.isActive());

    const char* recordMode = "idle";
    switch (m_record.mode()) {
        case RecordService::Mode::Command:   recordMode = "command";   break;
        default: break;
    }
    cJSON_AddStringToObject(data, "record_mode", recordMode);
    cJSON_AddStringToObject(data, "record_file", m_record.currentFilePath().c_str());

    /* 实时视频状态：模式（idle/p2p/relay）+ 观看人数 + 手动推流标志 */
    LiveStreamService::Status live = m_live.status();
    cJSON_AddStringToObject(data, "live_mode", live.mode.c_str());
    cJSON_AddNumberToObject(data, "live_viewers", live.viewers);
    cJSON_AddBoolToObject(data, "live_manual_push", live.manualPush);

    char* printed = cJSON_PrintUnformatted(data);
    std::string dataJson = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(data);
    return makeResponse(RespCode::Ok, "ok", dataJson);
}

/* 查当前生效的全量配置，和落盘的 mediad.json 内容一致（同一个 toJson）。
 * 不加锁直接读：本函数和改配置的 applyUpdate 都在 IPC 命令线程上
 * 一个接一个执行，读的时候不可能有人在写 */
std::string CommandRouter::handleGetConfig() {
    return makeResponse(RespCode::Ok, "ok", m_config.toJson());
}

/**
 * 拼标准应答 {"code","message","data"}，data 为空时不输出该字段
 *
 * dataJson 先解析再挂进去而不是字符串拼接：保证 data 在应答里
 * 是 JSON 对象而不是被转义的字符串；解析失败就丢弃 data 只回
 * code/message，不让坏 data 拖死整条应答。
 */
std::string CommandRouter::makeResponse(RespCode code, const std::string& message,
                                        const std::string& dataJson) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", static_cast<int>(code));
    cJSON_AddStringToObject(root, "message", message.c_str());
    if (!dataJson.empty()) {
        cJSON* data = cJSON_Parse(dataJson.c_str());
        if (data) {
            cJSON_AddItemToObject(root, "data", data);
        }
    }
    char* printed = cJSON_PrintUnformatted(root);
    /* 连应答都序列化不出来只可能是内存耗尽，回一条写死的错误包兜底，
     * 保证对端永远收得到应答 */
    std::string result = printed ? printed : "{\"code\":-5,\"message\":\"oom\"}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return result;
}

} // namespace mediad
