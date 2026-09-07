/**
 * @file MediadMessages.h
 * @brief mediad IPC 消息类型与协议定义
 *
 * mediad 在 lib-mq 用户自定义区间（10000-65535）内占用 12000-12999 段，
 * 遵循 message_types.h 的分段惯例（每类功能一个百位段）。
 *
 * 两条 IPC 通道（端点见 mediad.json 的 ipc 段）：
 *   1. 命令通道（REQ/REP，libmq::ReqRepQueue）——下行命令与配置，同步应答
 *      请求信封: {"type":<uint32>, "payload":{...}}
 *      应答信封: {"code":0,"message":"ok","data":{...}}   code!=0 表示失败
 *   2. 事件通道（PUB/SUB，libmq::MessageManager）——上行异步事件广播
 *      载荷为 JSON，消息类型走 MessageHeader.type
 *
 * 发送方：iot_live（MQTT→IPC 翻译）、mediactl（调试 CLI）、
 *         未来的蓝牙进程（tag 触发拍照）等——协议对所有发送方一致。
 */
#pragma once

#include "nc/common/file_utils.h"
#include "cJSON.h"

#include <cstdint>
#include <string>

namespace mediad {

/**
 * 命令类型（下行，REQ/REP 通道）
 *
 * 注：配置更新复用 lib-mq 内置 IpcMessageType::CONFIG_UPDATE(3000)，
 * 不在此重复定义（见 message_types.h "配置相关 3000-3999"段）。
 */
enum class Command : uint32_t {
    // ===== 拍照（12000） =====
    MEDIA_SNAPSHOT      = 12000,  ///< 立即拍照，payload 可选 {"quality":80}

    // ===== 录像（12001-12002，已废弃，录像由 triggers 系统控制） =====
    // MEDIA_RECORD_START = 12001,  // 已删除
    // MEDIA_RECORD_STOP  = 12002,  // 已删除

    // ===== 推流（12010-12011） =====
    MEDIA_STREAM_START  = 12010,  ///< 开始推流（SFU 模式，WHIP 推 SRS）
    MEDIA_STREAM_STOP   = 12011,  ///< 停止推流

    // ===== P2P（12020-12021，已废弃，P2P 由观看端 MQTT 信令直连，不经命令总线） =====
    // MEDIA_P2P_CONNECT    = 12020,  // 已删除
    // MEDIA_P2P_DISCONNECT = 12021,  // 已删除

    // ===== 查询（12030-12039） =====
    MEDIA_GET_STATUS    = 12030,  ///< 查询服务状态（相机/录像/定时任务）
    MEDIA_GET_CONFIG    = 12031,  ///< 查询当前生效的全量配置（data 即 mediad.json 内容）
};

/**
 * 事件类型（上行，PUB 通道广播）
 *
 * 已迁移到 libs/mq/include/libmq/message_types.h 的 IpcMessageType 枚举中，
 * 统一由 message_types.h 管理，避免两边定义不一致。
 * mediad 使用 libmq::IpcMessageType::MEDIA_FILE_READY 等。
 */

/**
 * 应答错误码（REQ/REP 信封的 code 字段）
 */
enum class RespCode : int {
    Ok             = 0,    ///< 成功
    BadRequest     = -1,   ///< 信封/参数不合法
    NotImplemented = -2,   ///< 功能未实现（推流/P2P 占位）
    Busy           = -3,   ///< 资源忙（如录像进行中收到新录像命令）
    CameraNotReady = -4,   ///< 相机未就绪（前置状态检查失败）
    Internal       = -5,   ///< 内部执行失败
};

} // namespace mediad

/* ---- MEDIA_FILE_READY 事件载荷构建 ---- */
namespace mediad {

/**
 * 组装文件就绪通知载荷
 *
 * 拍照/录像生成后，通过 IPC 广播给 iot_agent（iot_agent 转发上云）。
 * 这里只管“文件生成好了”这件事，不感知云端事件上报概念。
 * event_type / event_source / event_time 由 iot_agent 包装时添加。
 *
 * start_time/duration 仅录像恒带（服务器必填）：服务器靠它们算每个文件覆盖的
 * 时间范围（回放选文件用）；拍照不带这两个字段。
 */
inline std::string buildFileReadyPayload(const std::string& triggerType,
                                         const std::string& fileType,
                                         const std::string& path, int64_t size,
                                         const std::string& thumbPath, int64_t thumbSize,
                                         uint64_t timestamp,
                                         uint64_t startTimeSec = 0,
                                         uint64_t durationSec = 0) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "trigger_type", triggerType.c_str());
    cJSON_AddStringToObject(root, "file_type", fileType.c_str());
    cJSON_AddStringToObject(root, "file_name",
                            nc::common::ExtractFileName(path).c_str());
    cJSON_AddStringToObject(root, "file_path", path.c_str());
    cJSON_AddNumberToObject(root, "file_size", static_cast<double>(size));
    if (!thumbPath.empty()) {
        cJSON_AddStringToObject(root, "thumb_name",
                                nc::common::ExtractFileName(thumbPath).c_str());
        cJSON_AddStringToObject(root, "thumb_path", thumbPath.c_str());
        cJSON_AddNumberToObject(root, "thumb_size", static_cast<double>(thumbSize));
    }
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(timestamp));
    /* 录像专属字段（服务器必填，只要是视频就恒带，拍照不带）：
     * start_time 为 0 说明这段没写入任何帧（异常），退化为通知生成时间兆底 */
    if (fileType == "video") {
        cJSON_AddNumberToObject(root, "start_time",
                                static_cast<double>(startTimeSec > 0 ? startTimeSec : timestamp));
        cJSON_AddNumberToObject(root, "duration", static_cast<double>(durationSec));
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return result;
}

} // namespace mediad
