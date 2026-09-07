/**
 * @file cloud_config_adapter.cpp
 * @brief 云端配置字段翻译器实现
 *
 * 用映射表驱动翻译，加新字段只需要在表里加一行，不用改逻辑代码。
 *
 * 映射表格式：
 *   { 云端字段名, mediad路径（用点号分隔） }
 *
 * 翻译流程：
 *   1. 解析云端 JSON
 *   2. 遍历每个字段，查映射表找对应的 mediad 路径
 *   3. 按路径创建嵌套 JSON 结构
 *   4. 没有映射的字段直接丢弃（未来可以改成透传）
 */

/**
 * Cloud config field translator implementation
 *
 * Driven by a mapping table — adding a new field only requires one more table entry,
 * no logic code changes.
 *
 * Table format:
 *   { cloud_field_name, mediad_path (dot-separated) }
 *
 * Translation flow:
 *   1. Parse cloud JSON
 *   2. For each field, look up the mapping table to find the corresponding mediad path
 *   3. Build nested JSON structure along the path
 *   4. Fields without mapping are silently dropped (could be made pass-through in the future)
 */
#include "cloud_config_translator.h"

#include "nc/common/log_utils.h"
#include "cJSON.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace iot_agent {

/* ---- 映射表：云端字段名 → mediad 配置路径 ---- */
/* 加新字段只需要在这里加一行 */
struct FieldMapping {
    const char* cloudField;   /* 云端字段名 */
    const char* mediadPath;   /* mediad 路径，用点号分隔 */
};

static const FieldMapping kFieldMappings[] = {
    /* 主通道（录像/直播） */
    {"main_resolution_width",     "camera.main.width"},
    {"main_resolution_height",    "camera.main.height"},
    {"main_frame_rate",           "camera.main.fps"},
    {"main_bitrate",              "camera.main.bitrate_kbps"},
    {"main_codec",                "camera.main.codec"},
    {"main_br_mode",              "camera.main.br_mode"},
    
    /* 子通道（拍照/分析） */
    {"sub_resolution_width",      "camera.sub.width"},
    {"sub_resolution_height",     "camera.sub.height"},
    {"sub_codec",                 "camera.sub.codec"},
    {"sub_frame_rate",            "camera.sub.fps"},
    {"sub_bitrate",               "camera.sub.bitrate_kbps"},
    {"sub_br_mode",               "camera.sub.br_mode"},
    
    /* 拍照功能 */
    {"snapshot_quality",            "snapshot.quality"},
    
    /* OSD 水印 */
    {"osd_enable",                  "osd.enabled"},
    /* 元素数组：云端格式与 mediad 协议一致（千分比坐标），
     * 整个数组原样放到 osd.elements，设备端做校验和规范化 */
    {"osd_elements",                "osd.elements"},
    
    /* 录像分段时长 */
    {"record_segment_sec",          "record.segment_sec"},
    
    /* 实时流配置（推流/信令/ICE） */
    {"mqtt_signaling_url",          "live.mqtt_signaling.url"},
    {"push_stream_url",             "live.srs.whip_url_template"},
    {"ice_host",                    "live.ice.host"},
    {"ice_port",                    "live.ice.port"},
    {"ice_username",                "live.ice.username"},
    {"ice_password",                "live.ice.password"},
    
    /* 结束标记 */
    {nullptr, nullptr}
};

/* ---- 按路径设置 JSON 值 ---- */
/* 例如 path = "camera.sub.width"，就在 JSON 里创建 camera -> sub -> width */
static void setNestedValue(cJSON* root, const char* path, cJSON* value) {
    /* 把路径拆成段 */
    std::string pathStr(path);
    std::vector<std::string> segments;
    
    size_t start = 0;
    size_t dot = pathStr.find('.');
    while (dot != std::string::npos) {
        segments.push_back(pathStr.substr(start, dot - start));
        start = dot + 1;
        dot = pathStr.find('.', start);
    }
    segments.push_back(pathStr.substr(start));
    
    /* 逐层创建对象，最后一段放值 */
    cJSON* current = root;
    for (size_t i = 0; i < segments.size() - 1; ++i) {
        cJSON* child = cJSON_GetObjectItem(current, segments[i].c_str());
        if (!child || !cJSON_IsObject(child)) {
            /* 这一层还没有，创建一个新的对象 */
            child = cJSON_CreateObject();
            cJSON_AddItemToObject(current, segments[i].c_str(), child);
        }
        current = child;
    }
    
    /* 最后一段放值 */
    cJSON_AddItemToObject(current, segments.back().c_str(), value);
}

/* ---- 查找字段是否有映射 ---- */
static const char* findMediadPath(const char* cloudField) {
    for (const FieldMapping* m = kFieldMappings; m->cloudField != nullptr; ++m) {
        if (strcmp(m->cloudField, cloudField) == 0) {
            return m->mediadPath;
        }
    }
    return nullptr;
}

/* ====================================================================
 * translateCloudToMediad：把云端 delta 翻译成 mediad 格式
 * ==================================================================== */
std::string translateCloudToMediad(const std::string& cloudDelta) {
    cJSON* cloud = cJSON_Parse(cloudDelta.c_str());
    if (!cloud) {
        NC_LOGW("[CloudConfigAdapter] 云端 JSON 解析失败，原样返回");
        return cloudDelta;
    }
    
    cJSON* mediad = cJSON_CreateObject();
    int translatedCount = 0;
    
    /* 遍历云端每个字段，查映射表翻译 */
    for (cJSON* item = cloud->child; item; item = item->next) {
        if (!item->string) continue;
        
        /* triggers 字段特殊处理：直接透传，不翻译。
         * 云端下发的格式和 mediad 需要的格式一样，不需要转换 */
        if (strcmp(item->string, "triggers") == 0) {
            cJSON* triggersCopy = cJSON_Duplicate(item, 1);
            cJSON_AddItemToObject(mediad, "triggers", triggersCopy);
            ++translatedCount;
            NC_LOGD("[CloudConfigAdapter] triggers 字段直接透传");
            continue;
        }
        
        const char* mediadPath = findMediadPath(item->string);
        if (!mediadPath) {
            /* 没有映射的字段，跳过（未来可以改成透传） */
            NC_LOGD("[CloudConfigAdapter] 字段 {} 没有映射，跳过", item->string);
            continue;
        }
        
        /* 复制值，放到 mediad 的对应路径。
         * 防一手"云端把 JSON 数组存成字符串"：TB 控制台上配属性时，
         * 数组有时会被存成纯字符串；设备端只认真数组，不转的话
         * osd_elements 会静默丢（水印全没）。开头是 '[' 就试解析，
         * 解析失败保持原值，交给下游校验兜底 */
        cJSON* valueCopy = nullptr;
        if (cJSON_IsString(item) && item->valuestring) {
            const char* s = item->valuestring;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
            if (*s == '[') {
                valueCopy = cJSON_Parse(s);
            }
        }
        if (!valueCopy) {
            valueCopy = cJSON_Duplicate(item, 1);
        }
        setNestedValue(mediad, mediadPath, valueCopy);
        ++translatedCount;
        
        NC_LOGD("[CloudConfigAdapter] 翻译: {} -> {}", item->string, mediadPath);
    }
    
    /* 序列化结果 */
    char* result = cJSON_PrintUnformatted(mediad);
    std::string out = result ? result : cloudDelta;
    if (result) free(result);
    
    cJSON_Delete(cloud);
    cJSON_Delete(mediad);
    
    NC_LOGI("[CloudConfigAdapter] 翻译完成，{} 个字段", translatedCount);
    return out;
}

} // namespace iot_agent
