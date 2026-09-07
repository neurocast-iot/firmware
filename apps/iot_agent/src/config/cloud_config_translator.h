/**
 * @file cloud_config_translator.h
 * @brief 云端配置字段翻译器
 *
 * 云端用扁平字段名（如 snapshot_resolution_width），
 * 设备内部用嵌套结构（如 camera.sub.width）。
 * 这个类做翻译，让设备内部模块不需要关心云端命名。
 *
 * 设计原则：
 *   - 用映射表驱动，加字段只改表，不改代码
 *   - 放在 iot_agent 里，mediad 完全不知道云端字段叫什么
 */
#pragma once

#include <string>

namespace iot_agent {

/**
 * 把云端配置 delta 翻译成 mediad 的配置结构
 *
 * @param cloudDelta 云端字段格式的 JSON（如 {"snapshot_resolution_width": 768}）
 * @return mediad 配置格式的 JSON（如 {"camera":{"sub":{"width":768}}}）
 *
 * 如果云端 JSON 解析失败，原样返回输入
 */
std::string translateCloudToMediad(const std::string& cloudDelta);

} // namespace iot_agent
