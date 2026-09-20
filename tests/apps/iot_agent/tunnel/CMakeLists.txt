# ---------------------------------------------------------------------------
# iot_agent tunnel 模块单测
#
# 测什么:
#   TunnelManager —— 参数校验 / frpc 与 dbclient 命令构造 / 私钥落盘 /
#                    覆盖式重启 / 幂等停止 / 启动即失败的监控处理
# ---------------------------------------------------------------------------

include(GoogleTest)

add_executable(test_tunnel_manager
    test_tunnel_manager.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/tunnel/tunnel_manager.cpp
)
target_include_directories(test_tunnel_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src
)
target_link_libraries(test_tunnel_manager
    PRIVATE nc::cjson nc::common nc::compile_options
            GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_tunnel_manager PROPERTIES TIMEOUT 60)
