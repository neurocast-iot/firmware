/**
 * @file manifest_parser.h
 * @brief manifest.json 解析器
 *
 * manifest.json 描述升级包内容，格式示例：
 *   {
 *     "version": "1.2.5",
 *     "base_dir": "/",
 *     "files": [
 *       {
 *         "path": "/usr/bin/iot_live",
 *         "diff_mode": false,
 *         "new_sha256": "abc123...",
 *         "new_size": 1234567,
 *         "patch": "files/usr/bin/iot_live"
 *       }
 *     ]
 *   }
 */
#pragma once

#include "ota_types.h"

#include <string>

namespace iot_agent {
namespace ota {

class ManifestParser {
public:
    /** 解析 manifest.json 文件；失败时 out_manifest.error_msg 有错误信息 */
    static bool parse(const std::string& manifest_path, SwManifest& out_manifest);
};

} // namespace ota
} // namespace iot_agent
