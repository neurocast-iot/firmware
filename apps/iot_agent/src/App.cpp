/**
 * @file App.cpp
 * @brief iot_agent 组装根实现
 *
 * 从 main.cpp 原样迁入的布线逻辑，按模块依赖顺序组织：
 *   配置 → 硬件 → 云通道 → 上传 → IPC → OTA → 配置路由/RPC → 回调连线 → 启动
 *
 * 关停顺序和启动相反（stopStack 倒序），保证：
 *   - 先停业务（ota/ipc/upload），不再产生新数据
 *   - 再停云通道，最后的上报还能发出去
 *   - 最后停硬件
 */
#include "App.h"

#include "nc/common/log_utils.h"

#include <unistd.h>

namespace iot_agent {

int App::init(const std::string& configPath) {
    /* ---- 1) 读配置 ---- */
    if (!m_config.load(configPath)) {
        NC_LOGW("config load failed ({}), using defaults", configPath.c_str());
    }

    /* ---- 2) DeviceService（初始化硬件、获取设备 ID） ---- */
    if (!m_deviceService.initialize(m_config)) {
        NC_LOGE("DeviceService init failed");
        return 1;
    }
    m_stopStack.push_back([this] { m_deviceService.shutdown(); });

    std::string deviceId = m_deviceService.getDeviceId(m_config);
    if (deviceId.empty()) {
        NC_LOGE("no device_id available");
        return 2;
    }

    /* ---- 3) CloudService（拿 token、拼 MQTT 参数） ---- */
    if (!m_cloudService.initialize(m_config, deviceId)) {
        NC_LOGE("CloudService init failed");
        return 2;
    }
    /* stop() 幂等，先压栈：让云通道比业务模块后停 */
    m_stopStack.push_back([this] { m_cloudService.stop(); });

    /* ---- 4) 文件上传服务 ---- */
    /* 分层组装：HttpClient → FileUploadClient → FileUploadService */
    m_httpClient = std::unique_ptr<nc::http::HttpClient>(
        new nc::http::HttpClient(nc::http::HttpClientConfig{}));
    m_uploadClient = std::unique_ptr<FileUploadClient>(
        new FileUploadClient(*m_httpClient));
    m_uploadService = std::unique_ptr<FileUploadService>(
        new FileUploadService(*m_uploadClient));
    m_uploadService->setDeviceUid(deviceId);

    /* 上传结果 → media_file_upload_result 事件上报云端 */
    m_uploadResultReporter = std::unique_ptr<UploadResultReporter>(
        new UploadResultReporter(m_cloudService));
    m_uploadService->setResultCallback(
        [this](const UploadOutcome& out) {
            m_uploadResultReporter->onUploadResult(out);
        });

    m_uploadService->start(UploadServiceConfig{});
    m_stopStack.push_back([this] { m_uploadService->stop(); });

    /* ---- 5) IPC 中心节点 + 事件订阅 ---- */
    m_ipcHub.setDeviceId(deviceId);  /* 下发配置时自动注入 device_id 给 mediad */
    if (m_ipcHub.start()) {
        m_stopStack.push_back([this] { m_ipcHub.stop(); });
    }

    if (m_ipcHub.manager()) {
        m_ipcEventHandler.setCloudService(&m_cloudService);
        m_ipcEventHandler.setUploadService(m_uploadService.get());
        m_ipcEventHandler.subscribe(*m_ipcHub.manager());
    }

    /* ---- 6) OTA ---- */
    m_otaManager.initialize(&m_cloudService, m_config.otaBaseDir());
    m_stopStack.push_back([this] { m_otaManager.stop(); });

    /* ---- 7) ConfigRouter / RpcHandler ---- */
    if (!m_configRouter.initialize()) {
        NC_LOGW("ConfigRouter init failed, using defaults");
    }

    /* uploadFile RPC 指令依赖上传服务 */
    m_rpcHandler.setUploadService(m_uploadService.get());

    /* startLiveStream/stopLiveStream → IPC 转发给 mediad */
    m_rpcHandler.setIpcCommandCallback(
        [this](uint32_t type, const std::string& payload) -> bool {
            return m_ipcHub.sendTo("mediad", type, payload);
        });

    /* reset RPC 指令依赖配置：清掉 device_config.json + token.json */
    m_rpcHandler.setConfig(&m_config);

    /* 属性推送处理器 */
    m_attributeHandler = std::unique_ptr<AttributeHandler>(
        new AttributeHandler(m_configRouter, m_ipcHub, *m_uploadService, m_otaManager));

    /* ---- 8) 回调连线 ---- */
    m_cloudService.onAttributes(
        [this](const std::string& rawJson) {
            m_attributeHandler->handle(rawJson);
        });

    m_cloudService.onCommand(
        [this](const RpcRequest& req) {
            m_rpcHandler.handleRpc(req);
        });

    m_rpcHandler.setPublishCallback(
        [this](const std::string& topic, const std::string& payload) -> bool {
            return m_cloudService.publish(topic, payload);
        });

    /* ---- 9) 启动云连接 ---- */
    if (!m_cloudService.start()) {
        NC_LOGE("CloudService start failed");
        return 3;
    }
    NC_LOGI("iot_agent started, waiting for signals...");
    return 0;
}

void App::shutdown() {
    NC_LOGI("iot_agent shutting down...");
    /* 倒序关停：先停业务，再停云通道，最后停硬件 */
    for (auto it = m_stopStack.rbegin(); it != m_stopStack.rend(); ++it) {
        (*it)();
    }
    NC_LOGI("iot_agent exiting");
}

void App::pollLocalUpgrade() {
    m_otaManager.pollLocalUpgrade();
}

} // namespace iot_agent
