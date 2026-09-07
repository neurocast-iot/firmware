/**
 * @file upload_result_reporter.h
 * @brief 上传结果上报器 —— 把 UploadOutcome 组装成云端事件并上报遥测
 *
 * 和 media_file_ready 同一套事件 schema（event_type/event_source/event_time）：
 *   media_file_ready         —— mediad 说"文件在设备上生成了"
 *   media_file_upload_result —— iot_agent 说"上传的结果"（成功失败都发，靠 ok 字段区分）
 *
 * 为什么单独一个类：
 *   事件 JSON 的组装是上传模块的业务细节，不该摊在 main.cpp 里；
 *   以后事件字段要加要改，只动这一个文件。
 */
#pragma once

#include "file_upload_service.h"

namespace iot_agent {

class CloudService;

class UploadResultReporter {
public:
    /** @param cloud 云端连接（用来 publishTelemetry，本类不负责销毁） */
    explicit UploadResultReporter(CloudService& cloud);

    /**
     * 上传结果回调入口：组装事件 JSON 并上报遥测
     * 直接传给 FileUploadService::setResultCallback 用
     */
    void onUploadResult(const UploadOutcome& out);

private:
    CloudService& m_cloud;
};

} // namespace iot_agent
