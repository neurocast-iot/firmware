#include "verifier/manifest_parser.h"

#include "nc/common/log_utils.h"

#include "cJSON.h"

#include <fstream>
#include <sstream>

namespace iot_agent {
namespace ota {

// 从 cJSON 节点读字符串字段（key 不存在或类型不对返回空串）
static std::string readStr(const cJSON* obj, const char* key) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : "";
}

static bool readBool(const cJSON* obj, const char* key, bool def = false) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    return def;
}

static size_t readSize(const cJSON* obj, const char* key, size_t def = 0) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) return static_cast<size_t>(it->valuedouble);
    return def;
}

bool ManifestParser::parse(const std::string& manifest_path, SwManifest& out_manifest) {
    out_manifest = SwManifest{};

    std::ifstream ifs(manifest_path);
    if (!ifs.is_open()) {
        out_manifest.error_msg = "无法打开 manifest.json: " + manifest_path;
        NC_LOGE("[Manifest] {}", out_manifest.error_msg.c_str());
        return false;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    ifs.close();

    cJSON* root = cJSON_Parse(buf.str().c_str());
    if (!root) {
        out_manifest.error_msg = "manifest.json 不是合法 JSON: " + manifest_path;
        NC_LOGE("[Manifest] {}", out_manifest.error_msg.c_str());
        return false;
    }

    out_manifest.version  = readStr(root, "version");
    out_manifest.base_dir = readStr(root, "base_dir");
    // 计数从 files[] 算, 不信 JSON 里声明的 file_count/diff_count/full_count (可能不一致)
    out_manifest.file_count = 0;
    out_manifest.diff_count = 0;
    out_manifest.full_count = 0;

    if (out_manifest.version.empty()) {
        out_manifest.error_msg = "manifest.json 缺少 version 字段";
        NC_LOGE("[Manifest] {}", out_manifest.error_msg.c_str());
        cJSON_Delete(root);
        return false;
    }

    const cJSON* files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (!cJSON_IsArray(files)) {
        out_manifest.error_msg = "manifest.json files 数组缺失或不是数组";
        NC_LOGE("[Manifest] {}", out_manifest.error_msg.c_str());
        cJSON_Delete(root);
        return false;
    }
    // 允许空数组 (一个什么都不该改的 manifest, file_count=0 是合法状态)

    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, files) {
        SwFileEntry fe;
        fe.path        = readStr(entry, "path");
        fe.diff_mode   = readBool(entry, "diff_mode", false);
        fe.old_sha256  = readStr(entry, "old_sha256");
        fe.new_sha256  = readStr(entry, "new_sha256");
        fe.new_size    = readSize(entry, "new_size");
        fe.patch       = readStr(entry, "patch");

        if (fe.path.empty() || fe.new_sha256.empty() || fe.patch.empty()) {
            NC_LOGW("[Manifest] 跳过无效文件条目（缺 path/new_sha256/patch）");
            continue;
        }
        out_manifest.files.push_back(fe);
    }

    cJSON_Delete(root);

    // 算三个计数 (从实际解析到的 files[] 推, 不信 JSON 头部声明)
    for (const auto& fe : out_manifest.files) {
        ++out_manifest.file_count;
        if (fe.diff_mode) ++out_manifest.diff_count;
        else ++out_manifest.full_count;
    }

    out_manifest.valid = true;  // 即使 file_count=0 也 valid, 表示 "无可升级内容"
    NC_LOGI("[Manifest] 解析成功 version={} files={} diff={} full={}",
            out_manifest.version.c_str(),
            out_manifest.file_count,
            out_manifest.diff_count,
            out_manifest.full_count);
    return true;
}

} // namespace ota
} // namespace iot_agent
