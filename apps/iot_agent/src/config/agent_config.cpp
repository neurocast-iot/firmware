/**
 * @file agent_config.cpp
 * @brief iot_agent 配置加载实现
 *
 * 读 /etc/iot_agent.json，把整个 JSON 原样保留给平台实现自己解析。
 * 同时抽出平台无关的通用字段：platform / device_id / zigbee_port / upload（上传服务参数）。
 */
#include "agent_config.h"

#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"
#include "cJSON.h"

#include <cstdio>

namespace iot_agent {

bool AgentConfig::load(const std::string& path) {
    std::string content;
    if (!nc::common::ReadFile(path, content)) {
        NC_LOGW("[AgentConfig] config file missing: {}, using defaults", path.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) {
        NC_LOGE("[AgentConfig] parse failed: {}", path.c_str());
        return false;
    }

    /* 平台名：默认 thingsboard，可改 emqx / aliyun_iot / ... */
    const cJSON* p = cJSON_GetObjectItemCaseSensitive(root, "platform");
    if (cJSON_IsString(p) && p->valuestring) m_platformName = p->valuestring;

    /* 设备 ID：配置里直接写死的情况（调试用），没有的话后面走 Zigbee 读 */
    const cJSON* did = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (cJSON_IsString(did) && did->valuestring) m_deviceId = did->valuestring;

    /* Zigbee 串口路径：默认 /dev/ttySAK1，可配置覆盖 */
    const cJSON* zp = cJSON_GetObjectItemCaseSensitive(root, "zigbee_port");
    if (cJSON_IsString(zp) && zp->valuestring) m_zigbeePort = zp->valuestring;

    /* OTA 基础目录：默认 /mnt/emmc/ota，可配置覆盖 */
    const cJSON* ota = cJSON_GetObjectItemCaseSensitive(root, "ota");
    if (cJSON_IsObject(ota)) {
        const cJSON* base = cJSON_GetObjectItemCaseSensitive(ota, "base_dir");
        if (cJSON_IsString(base) && base->valuestring) m_otaBaseDir = base->valuestring;
    }

    cJSON_Delete(root);

    /* 原样保留 JSON 字符串，交给平台实现自己解析（不同平台需要的字段不一样） */
    m_rawJson = content;
    NC_LOGI("[AgentConfig] loaded {}, platform={}, dataDir={}",
            path.c_str(), m_platformName.c_str(), m_dataDir.c_str());
    return true;
}

bool AgentConfig::reset() {
    bool ok = true;
    if (std::remove(configPath().c_str()) != 0) {
        NC_LOGW("[AgentConfig] 删除 {} 失败（可能本来就不存在）", configPath().c_str());
    } else {
        NC_LOGI("[AgentConfig] 已删除 {}", configPath().c_str());
    }
    if (std::remove(tokenPath().c_str()) != 0) {
        NC_LOGW("[AgentConfig] 删除 {} 失败（可能本来就不存在）", tokenPath().c_str());
    } else {
        NC_LOGI("[AgentConfig] 已删除 {}", tokenPath().c_str());
    }
    return ok;
}

} // namespace iot_agent
