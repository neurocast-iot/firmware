/**
 * @file tb_provision_stub.cpp
 * @brief tbProvision 桩实现
 *
 * x86 测试环境不编 tb_provision.cpp（它要起真实 MQTT 客户端走 provision），
 * 这里提供一个假实现，测试里不会调到 authenticate()，
 * 但链接器需要这个符号。
 */
#include <string>

namespace iot_agent {

bool tbProvision(const std::string&,
                 const std::string&,
                 std::string&) {
    return false;  /* 桩：测试里不调用认证流程 */
}

} // namespace iot_agent
