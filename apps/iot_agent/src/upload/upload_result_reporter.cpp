/**
 * @file upload_result_reporter.cpp
 * @brief 上传结果上报器实现
 */
#include "upload_result_reporter.h"
#include "cloud/cloud_service.h"

#include "cJSON.h"

#include <ctime>

namespace iot_agent {

UploadResultReporter::UploadResultReporter(CloudService& cloud) : m_cloud(cloud) {}

void UploadResultReporter::onUploadResult(const UploadOutcome& out) {
    cJSON* event = cJSON_CreateObject();
    if (!event) return;

    cJSON_AddStringToObject(event, "event_type", "media_file_upload_result");
    cJSON_AddStringToObject(event, "event_source", "iot_agent");
    cJSON_AddNumberToObject(event, "event_time", static_cast<double>(std::time(nullptr)));
    cJSON_AddStringToObject(event, "file_name", out.fileName.c_str());
    cJSON_AddStringToObject(event, "file_type", out.fileType.c_str());
    cJSON_AddNumberToObject(event, "file_size", static_cast<double>(out.fileSize));
    /* RPC 按需上传的任务带 file_id，云端用它关联是哪次请求的上传结果 */
    if (!out.fileId.empty()) {
        cJSON_AddStringToObject(event, "file_id", out.fileId.c_str());
    }
    cJSON_AddBoolToObject(event, "ok", out.ok ? cJSON_True : cJSON_False);
    cJSON_AddNumberToObject(event, "attempts", out.attempts);
    if (!out.remotePath.empty()) {
        cJSON_AddStringToObject(event, "remote_path", out.remotePath.c_str());
    }
    if (out.instantComplete) {
        cJSON_AddBoolToObject(event, "instant_complete", cJSON_True);
    }
    if (!out.errorMessage.empty()) {
        cJSON_AddStringToObject(event, "error", out.errorMessage.c_str());
    }

    char* printed = cJSON_PrintUnformatted(event);
    if (printed) {
        /* 遥测上报：主题由 CloudService 问平台适配器要，这里不写死 */
        m_cloud.publishTelemetry(printed);
        cJSON_free(printed);
    }
    cJSON_Delete(event);
}

} // namespace iot_agent
