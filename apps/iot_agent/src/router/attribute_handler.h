/**
 * @file attribute_handler.h
 * @brief 云端属性推送处理器
 *
 * 收到云端属性推送后，根据字段名分发到不同的处理逻辑：
 *   - 配置类字段 → ConfigRouter（存本地 + 下发 mediad）
 *   - 上传凭据字段 → FileUploadService（热更新）
 *   - OTA 字段（fw_title, sw_version 等）→ OtaManager（升级处理）
 *
 * 扩展方式：加新属性类型只需加一个处理方法，不改 main.cpp。
 */
#pragma once

#include <string>

namespace iot_agent {

class ConfigRouter;
class IpcHub;
class FileUploadService;
namespace ota { class OtaManager; }

class AttributeHandler {
public:
    AttributeHandler(ConfigRouter& configRouter,
                     IpcHub& ipcHub,
                     FileUploadService& uploadService,
                     ota::OtaManager& otaManager);

    /** 处理云端属性推送（根据字段名分发到不同处理逻辑） */
    void handle(const std::string& rawJson);

private:
    ConfigRouter& m_configRouter;
    IpcHub& m_ipcHub;
    FileUploadService& m_uploadService;
    ota::OtaManager& m_otaManager;
};

} // namespace iot_agent
