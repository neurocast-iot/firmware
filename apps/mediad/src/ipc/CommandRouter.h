/**
 * @file CommandRouter.h
 * @brief 命令路由器：IPC 命令信封解析与分发
 *
 * 命令总线的中枢：所有触发源（iot_live / mediactl / 蓝牙进程）发来的
 * 请求信封 {"type":N,"payload":{}} 在此解析，按分发表路由到能力服务，
 * 统一组装应答信封 {"code":0,"message":"ok","data":{}}。
 *
 * 开闭原则：新增命令只加一个 case 分支，新增触发源零改动。
 * 线程模型：仅在 IpcClient 的命令线程串行调用，无并发。
 */
#pragma once

#include "ipc/MediadMessages.h"

#include <string>
#include <cstring>

/* cJSON 前向声明（全局作用域），避免头文件外泄 C 库依赖 */
struct cJSON;

namespace mediad {

class ConfigStore;
class CameraService;
class OsdService;
class SnapshotService;
class RecordService;
class LiveStreamService;
class TriggerManager;

class CommandRouter {
public:
    CommandRouter(ConfigStore& config,
                  CameraService& camera,
                  OsdService& osd,
                  RecordService& record,
                  LiveStreamService& live,
                  TriggerManager& triggerManager)
        : m_config(config), m_camera(camera), m_osd(osd),
          m_record(record), m_live(live),
          m_triggerManager(triggerManager) {}

    /**
     * 处理一条请求（IpcClient 命令线程调用）
     * @param request 请求信封 JSON 文本
     * @return 应答信封 JSON 文本（任何输入都保证返回合法应答）
     */
    std::string dispatch(const std::string& request);

private:
    /* 各命令处理器：payload 可为 nullptr（信封无 payload 字段） */
    std::string handleSnapshot(const cJSON* payload);
    std::string handleStreamStart(const cJSON* payload);
    std::string handleStreamStop(const cJSON* payload);
    std::string handleGetStatus();
    std::string handleGetConfig();

    /* 应答信封组装（dataJson 为 JSON 对象文本，空串表示无 data） */
    static std::string makeResponse(RespCode code, const std::string& message,
                                    const std::string& dataJson = "");

    ConfigStore&       m_config;
    CameraService&     m_camera;
    OsdService&        m_osd;
    RecordService&     m_record;
    LiveStreamService& m_live;
    TriggerManager&    m_triggerManager;
};

} // namespace mediad
