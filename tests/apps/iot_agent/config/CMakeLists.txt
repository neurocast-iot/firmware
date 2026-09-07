# ---------------------------------------------------------------------------
# iot_agent config 模块单测
#
# 测什么:
#   1) CloudConfigTranslator —— 云端字段名 → mediad 嵌套路径翻译
#   2) AgentConfig           —— 配置加载/默认值/reset
# ---------------------------------------------------------------------------

include(GoogleTest)

set(_iot_agent_config_includes
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/config
)

# --- test_cloud_config_translator ---
add_executable(test_cloud_config_translator
    test_cloud_config_translator.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/config/cloud_config_translator.cpp
)
target_include_directories(test_cloud_config_translator PRIVATE ${_iot_agent_config_includes})
target_link_libraries(test_cloud_config_translator
    PRIVATE nc::cjson nc::common nc::compile_options
            GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_cloud_config_translator PROPERTIES TIMEOUT 30)

# --- test_agent_config ---
add_executable(test_agent_config
    test_agent_config.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/config/agent_config.cpp
)
target_include_directories(test_agent_config PRIVATE ${_iot_agent_config_includes})
target_link_libraries(test_agent_config
    PRIVATE nc::cjson nc::common nc::compile_options
            GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_agent_config PROPERTIES TIMEOUT 30)
