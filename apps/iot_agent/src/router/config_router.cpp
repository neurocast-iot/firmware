/**
 * @file config_router.cpp
 * @brief 配置管理器实现
 *
 * 参考 gw_av100 的 ConfigManager：
 *   - 启动时从本地文件加载配置（文件不存在就用默认值兜底）
 *   - 收到 MQTT 属性推送 → 检测 OTA 字段 → 合并业务配置 → 存文件
 *   - 原子写文件（先写 .tmp 再 rename），防止写到一半断电导致文件损坏
 *
 * 和原始 ConfigManager 的区别：
 *   - 原始用 DeviceConfig 结构体管理配置，这里直接存 JSON 字符串
 *     （因为 iot_agent 不需要理解每个配置字段的含义，只需要存下来、后续传给 mediad）
 *   - OTA 检测逻辑从 DeviceManager 搬过来了（fw_* / sw_* 字段检测）
 */
#include "config_router.h"

#include "nc/common/log_utils.h"
#include "cJSON.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace iot_agent {

/* ---- OTA 字段前缀判断：fw_* / sw_* 开头的不存到配置文件 ---- */
static bool isOtaKey(const char* key) {
    if (!key) return false;
    return (key[0] == 'f' && key[1] == 'w' && key[2] == '_') ||
           (key[0] == 's' && key[1] == 'w' && key[2] == '_');
}

/* ====================================================================
 * 构造 / 析构
 * ==================================================================== */
ConfigRouter::ConfigRouter() = default;
ConfigRouter::~ConfigRouter() = default;

/* ====================================================================
 * initialize：启动时从本地文件加载配置
 * ==================================================================== */
bool ConfigRouter::initialize(const std::string& configPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configPath = configPath;

    if (loadFromFile()) {
        m_loaded = true;
        NC_LOGI("[ConfigRouter] 本地配置加载成功: {} ({} bytes)",
                configPath.c_str(), m_configJson.size());
    } else {
        /* 文件不存在，配置为空，等联网后从 TB 拉数据覆盖 */
        NC_LOGW("[ConfigRouter] 本地配置文件不存在，等待联网后同步");
        m_configJson = "{}";
        m_loaded = true;
    }
    return true;
}

/* ====================================================================
 * handleAttributes：处理 MQTT 推送的属性 JSON
 *
 * 参考原始 DeviceManager::handleAttributeResponse / handleAttributeUpdate：
 *   1) 解析 JSON
 *   2) 把业务配置字段和当前值比较，只合并真正变了的字段
 *   3) 如果有变更，存到本地文件，返回变更字段的 JSON
 *
 * 注意：OTA 字段（fw_* / sw_*）已在 TBAdapter::parseMessage() 里检测，
 *       这里不再处理，只负责配置合并。
 * ==================================================================== */
std::string ConfigRouter::handleAttributes(const std::string& attributesJson) {
    cJSON* root = cJSON_Parse(attributesJson.c_str());
    if (!root) {
        NC_LOGW("[ConfigRouter] 属性 JSON 解析失败");
        return "";
    }

    /* ---- 1) 把业务字段和当前值比较，只合并真正变了的 ---- */
    cJSON* delta = cJSON_CreateObject();  /* 收集变更的字段 */
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        /* 解析当前配置 */
        cJSON* current = cJSON_Parse(m_configJson.c_str());
        if (!current) {
            current = cJSON_CreateObject();
        }

        /* 遍历推送的字段，跳过 OTA 字段，和当前值比较后决定是否替换 */
        for (cJSON* item = root->child; item; item = item->next) {
            if (!item->string) continue;
            if (isOtaKey(item->string)) continue;  /* OTA 字段不存到配置文件 */
            if (strcmp(item->string, "deleted") == 0) continue;  /* TB 属性删除通知，丢弃 */

            /* 拿当前值，和新值比较 */
            cJSON* oldVal = cJSON_GetObjectItem(current, item->string);
            if (oldVal && cJSON_Compare(oldVal, item, cJSON_True)) {
                /* 值一样，跳过，不算变更 */
                continue;
            }

            /* 值不一样（或者原来没有这个 key），替换 */
            cJSON_DeleteItemFromObject(current, item->string);
            cJSON_AddItemToObject(current, item->string, cJSON_Duplicate(item, 1));

            /* 把变更的字段加到 delta 里 */
            cJSON_AddItemToObject(delta, item->string, cJSON_Duplicate(item, 1));

            /* 打日志，方便排查 */
            char* valStr = cJSON_PrintUnformatted(item);
            NC_LOGI("[ConfigRouter] 配置变更: {} = {}", item->string, valStr ? valStr : "?");
            if (valStr) free(valStr);
        }

        /* 序列化新配置 */
        char* newJson = cJSON_PrintUnformatted(current);
        if (newJson) {
            m_configJson = newJson;
            free(newJson);
        }
        cJSON_Delete(current);
    }

    /* ---- 3) 如果有变更，存到本地文件 ---- */
    std::string deltaJson;
    if (cJSON_GetArraySize(delta) > 0) {
        char* d = cJSON_PrintUnformatted(delta);
        if (d) {
            deltaJson = d;
            free(d);
        }
        if (saveToFile()) {
            NC_LOGI("[ConfigRouter] 配置已保存到 {}", m_configPath.c_str());
        } else {
            NC_LOGE("[ConfigRouter] 保存配置到 {} 失败", m_configPath.c_str());
        }
    }
    cJSON_Delete(delta);
    cJSON_Delete(root);
    return deltaJson;
}

/* ====================================================================
 * getConfigJson：读取当前完整配置
 * ==================================================================== */
std::string ConfigRouter::getConfigJson() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_configJson;
}

/* ====================================================================
 * saveToFile：原子写文件（先写 .tmp 再 rename）
 *
 * 和原始 ConfigManager::atomicSaveFile 一样：
 *   先写到 .tmp 文件，再用 rename 覆盖原文件。
 *   rename 在同一文件系统上是原子操作，不怕写到一半断电导致文件损坏。
 * ==================================================================== */
bool ConfigRouter::saveToFile() const {
    const std::string tmpPath = m_configPath + ".tmp";

    /* 写临时文件 */
    {
        std::ofstream f(tmpPath);
        if (!f.is_open()) {
            NC_LOGE("[ConfigRouter] 无法打开临时文件 {}", tmpPath.c_str());
            return false;
        }
        f << m_configJson;
        f.flush();
    }

    /* rename 覆盖原文件（原子操作） */
    if (std::rename(tmpPath.c_str(), m_configPath.c_str()) != 0) {
        NC_LOGE("[ConfigRouter] rename {} -> {} 失败", tmpPath.c_str(), m_configPath.c_str());
        return false;
    }

    return true;
}

/* ====================================================================
 * loadFromFile：从本地文件加载配置
 * ==================================================================== */
bool ConfigRouter::loadFromFile() {
    std::ifstream f(m_configPath);
    if (!f.is_open()) {
        return false;
    }

    std::stringstream ss;
    ss << f.rdbuf();
    m_configJson = ss.str();

    /* 验证 JSON 格式 */
    cJSON* root = cJSON_Parse(m_configJson.c_str());
    if (!root) {
        NC_LOGW("[ConfigRouter] 配置文件 JSON 解析失败: {}", m_configPath.c_str());
        m_configJson.clear();
        return false;
    }
    cJSON_Delete(root);
    return true;
}

} // namespace iot_agent
