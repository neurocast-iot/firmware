#include "nc/common/json_utils.h"

#include "cJSON.h"

namespace nc {
namespace common {

// 从 cJSON 对象中获取字符串字段值（字段不存在或不是字符串时返回空串）
std::string JsonGetString(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

} // namespace common
} // namespace nc
