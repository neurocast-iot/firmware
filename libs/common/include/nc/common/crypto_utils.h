// nc::common 加密/哈希工具
#pragma once

#include <string>

namespace nc {
namespace common {

// 计算文件 SHA256，返回 64 字符小写十六进制字符串；失败返回空
// 用 OpenSSL 实现，8KB 分块读取，避免大文件整体载入内存
std::string CalculateFileSHA256(const std::string& file_path);

// 计算文件 MD5，返回 32 字符小写十六进制字符串；失败返回空
// 用 OpenSSL EVP 实现，8KB 分块读取，避免大文件整体载入内存
std::string CalculateFileMD5(const std::string& file_path);

} // namespace common
} // namespace nc
