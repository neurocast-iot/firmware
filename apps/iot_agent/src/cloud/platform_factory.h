/**
 * @file platform_factory.h
 * @brief 平台工厂：根据配置里的 platform 字段创建对应 CloudAdapter 实例
 *
 * 编译期裁剪：
 *   CMakeLists.txt 里设 IOT_AGENT_PLATFORM=thingsboard，
 *   编译时定义 PLATFORM_THINGSBOARD 宏，
 *   只有 TB 的 #include 和 new 代码会编进去，
 *   AWS/EMQX 的代码完全不进二进制。
 *
 * 加新平台时：
 *   1) 写一个 XxxAdapter 实现 CloudAdapter
 *   2) CMakeLists.txt 加 elif 分支
 *   3) 这里加 #elif defined(PLATFORM_XXX) 分支
 *   其他代码（main / ConfigRouter / RpcRouter）一行不动。
 */
#pragma once

#include "cloud/cloud_adapter.h"
#include <memory>
#include <string>

namespace iot_agent {

class PlatformFactory {
public:
    /**
     * 根据平台名创建对应适配器
     *
     * 注意：编译期只编了一个平台的代码，
     * 所以这里传的 name 主要是做运行时校验（防止配置写错）。
     *
     * @param name 平台名（"thingsboard" / "aws" / "emqx" / ...）
     * @return 适配器实例；不匹配返回 nullptr
     */
    static std::unique_ptr<CloudAdapter> create(const std::string& name);
};

} // namespace iot_agent
