/**
 * @file ota_result_reporter.h
 * @brief OTA 状态上报器 —— 把 OTA 升级状态组装成 JSON 并上报云端
 *
 * 和 UploadResultReporter 同一套设计思路：
 *   - 事件 JSON 的组装是 OTA 模块的业务细节，不该摊在 OtaManager 里
 *   - 以后事件字段要加要改，只动这一个文件
 *
 * 字段对齐 ThingsBoard：
 *   - sw_state / sw_title / sw_version / sw_error（软件升级）
 *   - fw_state / fw_title / fw_version / fw_error（固件升级）
 *   - 升级成功时附 current_sw_title / current_sw_version（云端 dashboard 展示用）
 */
#pragma once

#include "ota_types.h"

#include <string>

namespace iot_agent {

class CloudService;

namespace ota {

/**
 * @brief OTA 状态上报器
 *
 * 职责：把 OTA 升级状态组装成 JSON 并通过 publishTelemetry 上报云端
 */
class OtaResultReporter {
public:
    /** @param cloud 云端连接（用来 publishTelemetry，本类不负责销毁） */
    explicit OtaResultReporter(CloudService* cloud);

    /**
     * 上报 OTA 状态
     * @param type 升级类型（固件/软件）
     * @param title 升级包标题
     * @param version 版本号
     * @param state 状态字符串（DOWNLOADING/DOWNLOADED/VERIFIED/UPDATING/UPDATED/FAILED）
     * @param err 错误信息（可选）
     */
    void reportState(OtaPackageType type,
                     const std::string& title,
                     const std::string& version,
                     const char* state,
                     const std::string& err = "");

private:
    CloudService* m_cloud;
};

} // namespace ota
} // namespace iot_agent
