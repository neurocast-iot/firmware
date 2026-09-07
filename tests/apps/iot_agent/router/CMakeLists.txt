# ---------------------------------------------------------------------------
# iot_agent router 模块单测
#
# 测什么:
#   1) ConfigRouter —— 配置合并/变更检测/OTA 字段跳过/原子写
#   2) RpcHandler —— RPC 分发逻辑/参数校验/响应格式
# ---------------------------------------------------------------------------

include(GoogleTest)

set(_iot_agent_router_includes
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/router
)

# --- test_config_router ---
add_executable(test_config_router
    test_config_router.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/router/config_router.cpp
)
target_include_directories(test_config_router PRIVATE ${_iot_agent_router_includes})
target_link_libraries(test_config_router
    PRIVATE nc::cjson nc::common nc::compile_options
            GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_config_router PROPERTIES TIMEOUT 30)

# --- test_rpc_handler ---
# 拉 rpc_handler.cpp + agent_config.cpp（reset 指令依赖）
# FileUploadService 只用了头文件里的 UploadTask 结构体，不链接实现
add_executable(test_rpc_handler
    test_rpc_handler.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/router/rpc_handler.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/config/agent_config.cpp
    file_upload_service_stub.cpp  # 桩：绕过 FileUploadService 的真实实现
)
target_include_directories(test_rpc_handler PRIVATE ${_iot_agent_router_includes})
target_link_libraries(test_rpc_handler
    PRIVATE nc::cjson nc::common nc::compile_options
            GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_rpc_handler PROPERTIES TIMEOUT 30)
