/**
 * @file ota_result_reporter.cpp
 * @brief OTA 状态上报器实现
 */
#include "ota_result_reporter.h"
#include "cloud/cloud_service.h"

#include "nc/common/log_utils.h"
#include "cJSON.h"

namespace iot_agent {
namespace ota {

OtaResultReporter::OtaResultReporter(CloudService* cloud) : m_cloud(cloud) {}

void OtaResultReporter::reportState(OtaPackageType type,
                                     const std::string& title,
                                     const std::string& version,
                                     const char* state,
                                     const std::string& err) {
    if (!m_cloud || !state) return;

    cJSON* root = cJSON_CreateObject();
    if (!root) return;

    const std::string prefix = otaPrefix(type);
    cJSON_AddStringToObject(root, (prefix + "_state").c_str(), state);
    if (!title.empty()) {
        cJSON_AddStringToObject(root, (prefix + "_title").c_str(), title.c_str());
    }
    if (!version.empty()) {
        cJSON_AddStringToObject(root, (prefix + "_version").c_str(), version.c_str());
    }
    if (!err.empty()) {
        cJSON_AddStringToObject(root, (prefix + "_error").c_str(), err.c_str());
    }
    // 升级成功时：附带 current_* 字段，云端用来展示
    if (std::string(state) == "UPDATED") {
        cJSON_AddStringToObject(root, ("current_" + prefix + "_title").c_str(),
                                title.empty() ? version.c_str() : title.c_str());
        cJSON_AddStringToObject(root, ("current_" + prefix + "_version").c_str(), version.c_str());
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    /* 直接发 telemetry，不经过中间层 */
    std::string payload(json);
    free(json);

    bool ok = m_cloud->publishTelemetry(payload);
    if (ok) {
        NC_LOGI("[OtaResultReporter] state reported: {}", payload.c_str());
    } else {
        NC_LOGE("[OtaResultReporter] state report failed: {}", payload.c_str());
    }
}

} // namespace ota
} // namespace iot_agent
