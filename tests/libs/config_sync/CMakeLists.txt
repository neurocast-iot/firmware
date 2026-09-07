# ---------------------------------------------------------------------------
# config_sync:: 单元测试 (tests/libs/config_sync/)
#
# 测什么:
#   1) ServiceStartedMsg::toJson/fromJson   —— 服务启动消息序列化
#   2) ConfigUpdateMsg::toJson/fromJson     —— 配置更新消息序列化
#   3) ConfigAckMsg::toJson/fromJson        —— 配置确认消息序列化
#   4) ConfigRequestMsg::toJson/fromJson    —— 配置请求消息序列化
#   5) ConfigResponseMsg::toJson/fromJson   —— 配置响应消息序列化
#
# 纯逻辑，无硬件依赖，x86 直接跑
# ---------------------------------------------------------------------------

include(GoogleTest)

# --- test_config_sync_types: 消息类型序列化/反序列化 ---
add_executable(test_config_sync_types
    test_config_sync_types.cpp
)

target_link_libraries(test_config_sync_types
    PRIVATE
        nc::config_sync       # 消息类型定义
        nc::common            # NC_LOGx
        nc::compile_options   # -Wall -Wextra cxx_std_17
        GTest::gtest
        GTest::gtest_main
)

# 单测最多 10 秒（纯序列化，很快）
gtest_discover_tests(test_config_sync_types PROPERTIES TIMEOUT 10)
