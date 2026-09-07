/**
 * @file tb_provision.h
 * @brief ThingsBoard provision 流程 —— 用 deviceKey/Secret 换 accessToken
 *
 * 从 ThingsBoardPlatform 里拆出来的，因为 provision 是"启动前的一次性认证"，
 * 不是"运行时平台逻辑"，不应该和 configureMqtt/handleMessage 混在一起。
 *
 * 流程：
 *   1) 先查本地文件有没有存过 token（上次 provision 成功就存了）
 *   2) 没有 → 起临时 MQTT 客户端走 provision 换 token
 *   3) 换到后存本地文件，下次启动直接用
 */
#pragma once

#include <string>

namespace iot_agent {

/**
 * TB provision：拿 accessToken
 *
 * @param cfgJson  iot_agent 配置 JSON（需要 mqtt.url/provision_device_key/provision_device_secret）
 * @param deviceId 设备 ID（当 deviceName 用）
 * @param tokenOut 输出参数：拿到的 accessToken
 * @return 是否成功
 */
bool tbProvision(const std::string& cfgJson,
                 const std::string& deviceId,
                 std::string& tokenOut);

} // namespace iot_agent
