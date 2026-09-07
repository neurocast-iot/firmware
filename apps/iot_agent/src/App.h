/**
 * @file App.h
 * @brief iot_agent 组装根（composition root）
 *
 * 职责：
 *   按依赖顺序创建所有模块、连回调、管关停。
 *   main.cpp 只管进程级事项（日志、信号、主循环），业务组装全在这里。
 *
 * 和 mediad 的 App 对称：
 *   - mediad App 管"本地媒体"（相机/OSD/录像/拍照/触发源）
 *   - iot_agent App 管"云端通道"（MQTT/上传/OTA/配置路由/RPC）
 *
 * 声明顺序 = 构造依赖顺序（后者引用前者），勿调整。
 */
#pragma once

#include "config/agent_config.h"
#include "device/device_service.h"
#include "cloud/cloud_service.h"
#include "router/config_router.h"
#include "router/rpc_handler.h"
#include "router/attribute_handler.h"
#include "ipc/ipc_hub.h"
#include "ipc/ipc_event_handler.h"
#include "ota/ota_manager.h"
#include "upload/file_upload_service.h"
#include "upload/file_upload_client.h"
#include "upload/upload_result_reporter.h"
#include "nc/http/http_client.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iot_agent {

class App {
public:
    App() = default;
    ~App() = default;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /**
     * 初始化：按依赖顺序创建所有模块、连回调、启动云连接
     *
     * @param configPath 配置文件路径（默认 /etc/config/iot_agent.json）
     * @return 0=成功；非 0=致命错误（设备 ID 拿不到 / 云通道起不来）
     */
    int init(const std::string& configPath);

    /**
     * 优雅退出：按启动的逆序关停各模块
     * （先停业务 ota/ipc/upload，再停云通道，最后停硬件）
     */
    void shutdown();

    /** 主循环里每轮调一次：扫本地升级 inbox */
    void pollLocalUpgrade();

private:
    /* ---- 关停栈：每成功启动一个模块就把关停动作压栈，退出时倒序执行 ---- */
    std::vector<std::function<void()>> m_stopStack;

    /* ---- 成员按依赖顺序声明（后者引用前者），勿调整 ---- */

    /* 1) 配置 */
    AgentConfig     m_config;

    /* 2) 硬件设备（Zigbee / GPS） */
    DeviceService   m_deviceService;

    /* 3) 云通道 */
    CloudService    m_cloudService;

    /* 4) 文件上传链路：HttpClient → FileUploadClient → FileUploadService
     *    这三个需要构造参数，用 unique_ptr 延迟到 init() 里创建 */
    std::unique_ptr<nc::http::HttpClient>   m_httpClient;
    std::unique_ptr<FileUploadClient>       m_uploadClient;
    std::unique_ptr<FileUploadService>      m_uploadService;

    /* 5) 上传结果上报 */
    std::unique_ptr<UploadResultReporter>   m_uploadResultReporter;

    /* 6) IPC 中心节点 + 事件订阅 */
    IpcHub          m_ipcHub;
    IpcEventHandler m_ipcEventHandler;

    /* 7) OTA */
    ota::OtaManager m_otaManager;

    /* 8) 配置路由 + RPC 处理 */
    ConfigRouter    m_configRouter;
    RpcHandler      m_rpcHandler;

    /* 9) 属性推送分发（需要引用其他模块，用 unique_ptr 延迟创建） */
    std::unique_ptr<AttributeHandler> m_attributeHandler;
};

} // namespace iot_agent
