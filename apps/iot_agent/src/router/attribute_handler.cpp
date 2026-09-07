#include "attribute_handler.h"

#include "router/config_router.h"
#include "ipc/ipc_hub.h"
#include "upload/file_upload_service.h"
#include "ota/ota_manager.h"
#include "ota/ota_types.h"
#include "nc/common/json_utils.h"
#include "nc/common/log_utils.h"

#include "cJSON.h"

#include <ctime>

namespace iot_agent {

AttributeHandler::AttributeHandler(ConfigRouter& configRouter,
                                   IpcHub& ipcHub,
                                   FileUploadService& uploadService,
                                   ota::OtaManager& otaManager)
    : m_configRouter(configRouter)
    , m_ipcHub(ipcHub)
    , m_uploadService(uploadService)
    , m_otaManager(otaManager)
{
}

void AttributeHandler::handle(const std::string& rawJson) {
    /* 解析一次 JSON，所有处理共用 */
    cJSON* root = cJSON_Parse(rawJson.c_str());
    if (!root) {
        return;
    }

    // 1) 配置类字段 → ConfigRouter（存本地 + 下发 mediad）
    //    handleAttributes 返回变更字段的 JSON，空字符串表示没变更
    std::string delta = m_configRouter.handleAttributes(rawJson);

    /* 无论有没有业务配置变更，都要推一次配置给 mediad——
     * device_id 不在云端属性里，是 iot_agent 自己注入的，
     * 必须随配置下发 mediad 才能拼 WHIP URL */
    {
        uint64_t version = 0;
        const cJSON* timeItem = cJSON_GetObjectItemCaseSensitive(root, "last_updated_time");
        if (cJSON_IsNumber(timeItem)) {
            version = static_cast<uint64_t>(timeItem->valuedouble);
        }
        if (version == 0) {
            version = static_cast<uint64_t>(std::time(nullptr));
        }
        m_ipcHub.publishConfigDelta(version, delta);
    }

    // 2) 上传凭据字段 → FileUploadService（热更新）
    std::string baseUrl = nc::common::JsonGetString(root, "upload_file_base_url");
    std::string apiKey = nc::common::JsonGetString(root, "upload_file_api_key");
    if (!baseUrl.empty() || !apiKey.empty()) {
        m_uploadService.updateServerInfo(baseUrl, apiKey);
    }

    // 3) OTA 字段（fw_*/sw_*）→ OtaManager（升级处理）
    // ThingsBoard OTA 字段：fw_title, fw_version, fw_url, fw_checksum, fw_size, fw_checksum_algorithm
    //                    sw_title, sw_version, sw_url, sw_checksum, sw_size, sw_checksum_algorithm
    for (const char* prefix : {"fw", "sw"}) {
        std::string versionKey = std::string(prefix) + "_version";
        std::string urlKey = std::string(prefix) + "_url";
        
        cJSON* versionItem = cJSON_GetObjectItem(root, versionKey.c_str());
        if (cJSON_IsString(versionItem) && versionItem->valuestring && versionItem->valuestring[0]) {
            OtaNotice notice;
            notice.type = prefix;
            notice.title = nc::common::JsonGetString(root, (std::string(prefix) + "_title").c_str());
            notice.version = versionItem->valuestring;
            notice.url = nc::common::JsonGetString(root, urlKey.c_str());
            notice.checksum = nc::common::JsonGetString(root, (std::string(prefix) + "_checksum").c_str());
            notice.checksumAlgorithm = nc::common::JsonGetString(root, (std::string(prefix) + "_checksum_algorithm").c_str());
            
            cJSON* sizeItem = cJSON_GetObjectItem(root, (std::string(prefix) + "_size").c_str());
            if (cJSON_IsNumber(sizeItem)) {
                notice.size = sizeItem->valueint;
            }
            
            NC_LOGI("[AttributeHandler] OTA detected: type={} version={} url={}",
                     notice.type.c_str(), notice.version.c_str(), notice.url.c_str());
            m_otaManager.onOtaNotice(notice);
            break;  // 只处理一个 OTA 类型
        }
    }

    cJSON_Delete(root);
}

} // namespace iot_agent
