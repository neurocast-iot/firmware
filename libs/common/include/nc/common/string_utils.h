// nc::common 字符串工具
#pragma once

#include <string>

namespace nc {
namespace common {

// 去除字符串首尾空白（空格、制表符、换行符、回车符）
std::string TrimString(const std::string& s);

// 从 URL 末尾提取文件名（先去掉 ?query 和 #fragment，再取最后一个 / 后面的部分）
// 比如 "http://example.com/path/file.tar.gz?token=abc" → "file.tar.gz"
std::string ExtractFileNameFromUrl(const std::string& url);

// URL 百分号编码（把空格、中文等字符编码成 %20、%E4%BD%... 等）
// 用于拼 query 参数，文件名可能含特殊字符时必须编码
std::string UrlEncode(const std::string& raw);

} // namespace common
} // namespace nc
