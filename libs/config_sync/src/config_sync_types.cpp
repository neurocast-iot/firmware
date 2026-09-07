/**
 * @file config_sync_types.cpp
 * @brief 配置同步消息格式的 JSON 序列化/反序列化
 */
#include "nc/config_sync/config_sync_types.h"

#include "cJSON.h"

#include <cstring>

namespace nc {
namespace config_sync {

namespace {

/* 辅助函数：从 cJSON 对象获取字符串字段，不存在则返回空字符串 */
std::string getStringField(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return "";
}

/* 辅助函数：从 cJSON 对象获取数字字段，不存在则返回默认值 */
uint64_t getNumberField(const cJSON* obj, const char* key, uint64_t defaultVal = 0) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return static_cast<uint64_t>(item->valuedouble);
    }
    return defaultVal;
}

/* 辅助函数：从 cJSON 对象获取布尔字段，不存在则返回默认值 */
bool getBoolField(const cJSON* obj, const char* key, bool defaultVal = false) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return defaultVal;
}

/* 辅助函数：将 cJSON 对象序列化为字符串 */
std::string cJSONToString(cJSON* json) {
    char* str = cJSON_PrintUnformatted(json);
    std::string result(str);
    free(str);
    return result;
}

/* 辅助函数：获取 JSON 中的 config 字段并序列化为字符串 */
std::string getConfigAsString(const cJSON* obj) {
    const cJSON* config = cJSON_GetObjectItemCaseSensitive(obj, "config");
    if (config == nullptr) {
        return "{}";
    }
    if (cJSON_IsString(config)) {
        return config->valuestring ? config->valuestring : "{}";
    }
    /* config 是对象，序列化为字符串 */
    return cJSONToString(const_cast<cJSON*>(config));
}

} // namespace

// ===== ServiceStartedMsg =====

std::string ServiceStartedMsg::toJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "serviceName", serviceName.c_str());
    cJSON_AddNumberToObject(root, "configVersion", static_cast<double>(configVersion));
    std::string result = cJSONToString(root);
    cJSON_Delete(root);
    return result;
}

ServiceStartedMsg ServiceStartedMsg::fromJson(const std::string& json) {
    ServiceStartedMsg msg;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return msg;
    }
    msg.serviceName = getStringField(root, "serviceName");
    msg.configVersion = getNumberField(root, "configVersion");
    cJSON_Delete(root);
    return msg;
}

// ===== ConfigUpdateMsg =====

std::string ConfigUpdateMsg::toJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", static_cast<double>(version));
    /* config 字段直接嵌入 JSON 对象 */
    cJSON* configJson = cJSON_Parse(config.c_str());
    if (configJson != nullptr) {
        cJSON_AddItemToObject(root, "config", configJson);
    } else {
        /* config 解析失败，作为空对象 */
        cJSON_AddObjectToObject(root, "config");
    }
    std::string result = cJSONToString(root);
    cJSON_Delete(root);
    return result;
}

ConfigUpdateMsg ConfigUpdateMsg::fromJson(const std::string& json) {
    ConfigUpdateMsg msg;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return msg;
    }
    msg.version = getNumberField(root, "version");
    msg.config = getConfigAsString(root);
    cJSON_Delete(root);
    return msg;
}

// ===== ConfigAckMsg =====

std::string ConfigAckMsg::toJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", static_cast<double>(version));
    cJSON_AddBoolToObject(root, "success", success);
    if (!error.empty()) {
        cJSON_AddStringToObject(root, "error", error.c_str());
    }
    std::string result = cJSONToString(root);
    cJSON_Delete(root);
    return result;
}

ConfigAckMsg ConfigAckMsg::fromJson(const std::string& json) {
    ConfigAckMsg msg;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return msg;
    }
    msg.version = getNumberField(root, "version");
    msg.success = getBoolField(root, "success");
    msg.error = getStringField(root, "error");
    cJSON_Delete(root);
    return msg;
}

// ===== ConfigRequestMsg =====

std::string ConfigRequestMsg::toJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "serviceName", serviceName.c_str());
    std::string result = cJSONToString(root);
    cJSON_Delete(root);
    return result;
}

ConfigRequestMsg ConfigRequestMsg::fromJson(const std::string& json) {
    ConfigRequestMsg msg;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return msg;
    }
    msg.serviceName = getStringField(root, "serviceName");
    cJSON_Delete(root);
    return msg;
}

// ===== ConfigResponseMsg =====

std::string ConfigResponseMsg::toJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", static_cast<double>(version));
    /* config 字段直接嵌入 JSON 对象 */
    cJSON* configJson = cJSON_Parse(config.c_str());
    if (configJson != nullptr) {
        cJSON_AddItemToObject(root, "config", configJson);
    } else {
        cJSON_AddObjectToObject(root, "config");
    }
    std::string result = cJSONToString(root);
    cJSON_Delete(root);
    return result;
}

ConfigResponseMsg ConfigResponseMsg::fromJson(const std::string& json) {
    ConfigResponseMsg msg;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return msg;
    }
    msg.version = getNumberField(root, "version");
    msg.config = getConfigAsString(root);
    cJSON_Delete(root);
    return msg;
}

} // namespace config_sync
} // namespace nc
