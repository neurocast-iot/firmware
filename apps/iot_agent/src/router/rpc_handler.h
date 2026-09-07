/**
 * @file rpc_handler.h
 * @brief RPC 指令处理器
 *
 * 职责：
 *   收到 TB 推送的 RPC 指令 → 按 method 分发处理 → 回复 TB 执行结果
 *
 * 支持的指令：
 *   - uploadFile：上传设备上的原始文件（按需拉取场景）
 *     参数：{"fileType":"image|video","filePath":"/mnt/...","fileId":"xxx"}
 *     收到指令后入队上传任务，实际上传结果由 media_file_upload_result 事件
 *     上报云端（响应只表示任务已入队）
 *   - startLiveStream：开始实时流推送
 *     参数：无（或空 JSON）
 *     通过 IPC 发 MEDIA_STREAM_START 给 mediad，触发 LiveStreamService 进入推流模式
 *   - stopLiveStream：停止实时流推送
 *     参数：无（或空 JSON）
 *     通过 IPC 发 MEDIA_STREAM_STOP 给 mediad
 *   - restart：重启设备
 *     参数：无
 *     先回复 TB 成功，1 秒后执行 reboot
 *   - reset：恢复出厂配置
 *     参数：无
 *     清掉 device_config.json + token.json，回复成功后自动重启设备
 *
 * 回复方式：
 *   TB 的 RPC 回复要 publish 到 v1/devices/me/rpc/response/{requestId}
 *   所以需要一个 publish 回调（由 main.cpp 传入，底层调 MqttClient::publish）
 */

/**
 * RPC command handler
 *
 * Responsibility:
 *   Receive TB RPC commands -> dispatch by method -> reply with execution result
 *
 * Supported commands:
 *   - uploadFile: upload raw files on device (on-demand retrieval scenario)
 *     Params: {"fileType":"image|video","filePath":"/mnt/...","fileId":"xxx"}
 *     Enqueues upload task; actual result reported via media_file_upload_result event
 *     (response only indicates task was enqueued)
 *   - startLiveStream: start live stream push
 *     Params: none (or empty JSON)
 *     Sends MEDIA_STREAM_START via IPC to mediad, triggers LiveStreamService push mode
 *   - stopLiveStream: stop live stream push
 *     Params: none (or empty JSON)
 *     Sends MEDIA_STREAM_STOP via IPC to mediad
 *   - restart: reboot device
 *     Params: none
 *     Reply TB success first, then reboot after 1 second
 *   - reset: factory reset
 *     Params: none
 *     Clear device_config.json + token.json, auto-reboot after reply success
 *
 * Reply method:
 *   TB RPC replies must be published to v1/devices/me/rpc/response/{requestId}
 *   Requires a publish callback (passed in by main.cpp,底层 calls MqttClient::publish)
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace iot_agent {

class AgentConfig;
class FileUploadService;

/**
 * 平台无关的 RPC 指令（由平台实现解析出来）
 */
struct RpcRequest {
    std::string method;         /* 方法名（如 "setCameraConfig"） */
    std::string paramsJson;     /* 参数 JSON 字符串 */
    std::string requestId;      /* 请求 ID（TB 是数字 ID） */
    std::string source;         /* 来源平台名（"thingsboard" / "emqx"） */
};

class RpcHandler {
public:
    /* publish 回调：用来回复 TB RPC 结果 */
    using PublishCallback = std::function<bool(const std::string& topic, const std::string& payload)>;

    RpcHandler();
    ~RpcHandler();

    /**
     * 设置 publish 回调（由 main.cpp 在 MQTT 连接好后传入）
     */
    void setPublishCallback(PublishCallback cb);

    /**
     * 设置 IPC 命令发送回调（由 main.cpp 传入，用来发命令给 mediad）
     *
     * 回调参数：(命令类型编号, 载荷 JSON)
     * 底层由 main.cpp 调 ipcHub->sendTo("mediad", type, payload) 实现
     */
    using IpcCommandCallback = std::function<bool(uint32_t type, const std::string& payload)>;
    void setIpcCommandCallback(IpcCommandCallback cb);

    /**
     * 设置文件上传服务（uploadFile 指令依赖，不设置则该指令直接报错）
     */
    void setUploadService(FileUploadService* service);

    /**
     * 设置配置（reset 指令依赖，用来删配置文件 + token）
     */
    void setConfig(AgentConfig* config);

    /**
     * 处理一条 RPC 指令：按 method 分发到具体处理逻辑
     */
    void handleRpc(const RpcRequest& req);

private:
    /** uploadFile 指令处理：解析参数 → 校验文件 → 入队上传 → 回复 */
    void handleUploadFile(const RpcRequest& req);

    /** startLiveStream 指令处理：通过 IPC 通知 mediad 开始推流 */
    void handleStartLiveStream(const RpcRequest& req);

    /** stopLiveStream 指令处理：通过 IPC 通知 mediad 停止推流 */
    void handleStopLiveStream(const RpcRequest& req);

    /** restart 指令处理：回复成功后延迟重启设备 */
    void handleRestart(const RpcRequest& req);

    /** reset 指令处理：清掉配置文件和 token，回复成功后自动重启 */
    void handleReset(const RpcRequest& req);

    /** 回复 TB RPC 结果 */
    void sendResponse(const std::string& requestId, const std::string& resultJson);

    PublishCallback m_publish;
    IpcCommandCallback m_ipcCommand;
    FileUploadService* m_uploadService = nullptr;
    AgentConfig* m_config = nullptr;
};

} // namespace iot_agent
