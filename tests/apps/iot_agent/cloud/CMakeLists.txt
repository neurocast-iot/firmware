# ---------------------------------------------------------------------------
# iot_agent cloud 模块单测
#
# 测什么:
#   1) TokenStore —— token 文件持久化（保存/加载/边界条件）
#   2) TBAdapter —— parseMessage 消息解析 + configureMqtt 配置映射
# ---------------------------------------------------------------------------

include(GoogleTest)

set(_iot_agent_cloud_includes
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/cloud/thingsboard
)
# --- test_token_store ---
add_executable(test_token_store
    test_token_store.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/cloud/thingsboard/token_store.cpp
)
target_include_directories(test_token_store PRIVATE ${_iot_agent_cloud_includes})
target_link_libraries(test_token_store
    PRIVATE nc::cjson nc::common nc::compile_options
            GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_token_store PROPERTIES TIMEOUT 30)

# --- test_tb_adapter ---
# 桩头文件（stubs/）提供 MqttConfig/MqttState/MqttClient 类型声明，
# 放在 include 路径最前面，这样桩版本的 nc/mqtt/mqtt_client.h
# 会盖住源码树里的真实头文件，绕过 Paho 依赖。
add_executable(test_tb_adapter
    test_tb_adapter.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/cloud/thingsboard/tb_adapter.cpp
    tb_provision_stub.cpp  # 桩：绕过 tb_provision 的真实 MQTT 连接
)
target_include_directories(test_tb_adapter
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs   # 桩头文件优先
        ${_iot_agent_cloud_includes}
)
target_link_libraries(test_tb_adapter
    PRIVATE nc::cjson nc::common nc::compile_options
            GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_tb_adapter PROPERTIES TIMEOUT 30)
