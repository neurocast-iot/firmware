#include "nc/common/string_utils.h"

#include <cstdio>

namespace nc {
namespace common {

// 去除字符串首尾空白（空格、制表符、换行符、回车符）
std::string TrimString(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// 从 URL 末尾提取文件名（先去掉 ?query 和 #fragment，再取最后一个 / 后面的部分）
std::string ExtractFileNameFromUrl(const std::string& url) {
    std::string u = url;
    auto q = u.find('?');
    if (q != std::string::npos) u = u.substr(0, q);
    auto h = u.find('#');
    if (h != std::string::npos) u = u.substr(0, h);
    auto slash = u.find_last_of('/');
    return (slash == std::string::npos) ? u : u.substr(slash + 1);
}

// URL 百分号编码（只对非 unreserved 字符编码，RFC 3986）
// unreserved = ALPHA / DIGIT / '-' / '.' / '_' / '~'
std::string UrlEncode(const std::string& raw) {
    std::string result;
    result.reserve(raw.size() * 2);  // 最坏情况每个字符都编码
    for (unsigned char c : raw) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            result += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            result += buf;
        }
    }
    return result;
}

} // namespace common
} // namespace nc
