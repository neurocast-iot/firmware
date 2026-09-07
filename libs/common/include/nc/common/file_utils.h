// nc::common 文件工具
// TODO(第2步): 迁入 ota_agent/iot_monitor 现有 file_utils 实现并统一
#pragma once

#include <cstdint>
#include <string>

namespace nc {
namespace common {

// 文件是否存在
bool FileExists(const std::string& path);

// 读取整个文件内容；失败返回 false
bool ReadFile(const std::string& path, std::string& content);

// 原子写文件（先写临时文件再 rename）
bool WriteFileAtomic(const std::string& path, const std::string& content);

// 文件大小（字节）；失败返回 -1
int64_t FileSize(const std::string& path);

// 递归创建目录
bool MakeDirs(const std::string& path);

// 复制文件（POSIX read/write，固定 4KB 缓冲区，写入失败自动删除半成品）
bool CopyFile(const std::string& srcPath, const std::string& dstPath);

// 从路径中提取文件名（如 "/mnt/photos/xxx.jpg" → "xxx.jpg"）
std::string ExtractFileName(const std::string& path);

// 从源文件路径推缩略图路径：去后缀 + "_thumb.jpg"（如 "/dir/xxx.mp4" → "/dir/xxx_thumb.jpg"）
// 录像和拍照共用，避免两处各写一遍产生漂移
std::string ThumbPathFromSource(const std::string& sourcePath);

} // namespace common
} // namespace nc
