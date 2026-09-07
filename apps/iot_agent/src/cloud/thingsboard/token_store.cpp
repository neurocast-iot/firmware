/**
 * @file token_store.cpp
 * @brief TB token 本地持久化实现
 *
 * 文件格式（JSON）：
 *   {"token":"xxx","timestamp":1234567890}
 *
 * 为什么不直接存纯字符串：
 *   留 timestamp 字段是为了以后能做"token 过期 / 强制刷新"逻辑
 *   （虽然现在 TB 的 accessToken 不过期，但 EMQX 的 JWT 会过期，留个口子）
 */
#include "token_store.h"

#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"
#include "cJSON.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>

namespace iot_agent {

bool TokenStore::load(const std::string& path, std::string& tokenOut) {
    std::string content;
    if (!nc::common::ReadFile(path, content)) {
        return false;   /* 文件不存在或读不到，上层走 provision */
    }

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) {
        NC_LOGW("[TokenStore] parse failed, treat as missing: {}", path.c_str());
        return false;
    }

    cJSON* t = cJSON_GetObjectItem(root, "token");
    if (!cJSON_IsString(t) || !t->valuestring || t->valuestring[0] == '\0') {
        NC_LOGW("[TokenStore] token field empty, treat as missing");
        cJSON_Delete(root);
        return false;
    }

    tokenOut = t->valuestring;
    cJSON_Delete(root);
    return true;
}

bool TokenStore::save(const std::string& path, const std::string& token) {
    if (token.empty()) {
        NC_LOGE("[TokenStore] refuse to save empty token");
        return false;
    }

    /* 确保父目录存在（/data/iot_agent/ 可能还没建） */
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        nc::common::MakeDirs(path.substr(0, slash));
    }

    /* 拼 JSON：{"token":"xxx","timestamp":...} */
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "token", token.c_str());
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(ts));

    char* jsonStr = cJSON_PrintUnformatted(root);
    std::string content = jsonStr ? jsonStr : "";
    free(jsonStr);
    cJSON_Delete(root);

    if (content.empty()) return false;

    /* 写文件（原子写：先写临时文件再 rename，避免写到一半断电留个空文件） */
    if (!nc::common::WriteFileAtomic(path, content)) {
        NC_LOGE("[TokenStore] write failed: {}", path.c_str());
        return false;
    }
    NC_LOGI("[TokenStore] token saved to {} (len={})", path.c_str(), token.size());
    return true;
}

} // namespace iot_agent
