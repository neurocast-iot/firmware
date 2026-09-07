// nc::common cJSON 工具
#pragma once

#include <string>

struct cJSON;

namespace nc {
namespace common {

// 从 cJSON 对象中获取字符串字段值（字段不存在或不是字符串时返回空串）
std::string JsonGetString(const cJSON* obj, const char* key);

} // namespace common
} // namespace nc
