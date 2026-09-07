# ---------------------------------------------------------------------------
# iot_agent OTA 模块单测 (tests/apps/iot_agent/ota/)
#
# 测什么:
#   1) ManifestParser    —— manifest.json 合法/非法/缺字段
#                            实现: apps/iot_agent/src/ota/verifier/manifest_parser.cpp
#   2) OtaTypes           —— parseOtaType / otaPrefix / otaTypeName 等 inline 工具函数
#                            实现: apps/iot_agent/src/ota/ota_types.h (header-only)
#   3) OtaConfig          —— OtaPaths 路径拼接 / OtaConstants 常量
#                            实现: apps/iot_agent/src/ota/ota_config.h (header-only)
#   4) BackupManager      —— 备份/恢复/清理旧备份
#                            实现: apps/iot_agent/src/ota/backup_manager.cpp
#
# 为什么把 iot_agent 的 .cpp 拉进测试:
#   - 这些 .cpp 没有 main() 入口, 不会冲突
#   - 重编译 iot_agent 整体反而慢, 而且要拉一堆依赖 (Paho MQTT, 上传服务, etc.)
#   - 测的是函数行为, 不需要进程上下文
#
# 注意:
#   - SHA256 测试已移到 tests/libs/common/test_crypto_utils.cpp
#   - OTA 类型解析测试已移到 tests/libs/common/ (如果有的话)
# ---------------------------------------------------------------------------

include(GoogleTest)

# 公共 include 路径: 测试代码需要 include iot_agent 自己的业务头文件
set(_iot_agent_ota_includes
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/ota
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/ota/verifier
)

# --- test_manifest_parser ---
add_executable(test_manifest_parser
    test_manifest_parser.cpp
    # iot_agent 业务逻辑实现: 不重编 iot_agent 主体, 只拉被测的 .cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/ota/verifier/manifest_parser.cpp
)

target_include_directories(test_manifest_parser PRIVATE ${_iot_agent_ota_includes})

target_link_libraries(test_manifest_parser
    PRIVATE
        nc::cjson            # ManifestParser 用 cJSON_Parse
        nc::common           # NC_LOGx
        nc::compile_options
        GTest::gtest
        GTest::gtest_main
)

gtest_discover_tests(test_manifest_parser PROPERTIES TIMEOUT 30)

# --- test_ota_types ---
# ota_types.h 全是 inline 函数, 不需要拉 .cpp 重编译
add_executable(test_ota_types
    test_ota_types.cpp
)
target_include_directories(test_ota_types PRIVATE ${_iot_agent_ota_includes})
target_link_libraries(test_ota_types
    PRIVATE GTest::gtest GTest::gtest_main
)
gtest_discover_tests(test_ota_types PROPERTIES TIMEOUT 30)

# --- test_ota_config ---
# ota_config.h 全是 static inline / constexpr, 不需要拉 .cpp 重编译
add_executable(test_ota_config
    test_ota_config.cpp
)
target_include_directories(test_ota_config PRIVATE ${_iot_agent_ota_includes})
target_link_libraries(test_ota_config
    PRIVATE GTest::gtest GTest::gtest_main
)
gtest_discover_tests(test_ota_config PROPERTIES TIMEOUT 30)

# --- test_backup_manager ---
add_executable(test_backup_manager
    test_backup_manager.cpp
    ${CMAKE_SOURCE_DIR}/apps/iot_agent/src/ota/backup_manager.cpp
)
target_include_directories(test_backup_manager PRIVATE ${_iot_agent_ota_includes})
target_link_libraries(test_backup_manager
    PRIVATE nc::common nc::compile_options
            GTest::gtest GTest::gtest_main
)
gtest_discover_tests(test_backup_manager PROPERTIES TIMEOUT 30)
