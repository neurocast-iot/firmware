# ---------------------------------------------------------------------------
# mediad config 模块单测
#
# 测什么:
#   1) ConfigStore —— 配置加载/合并/校验/拒绝流程
#   2) 各配置段阈值范围校验（ChannelCfg/CameraCfg/SnapshotCfg/ThumbnailCfg/RecordCfg）
#   3) OSD 规范化（千分比钳位、字号回落、坏元素剔除）
#   4) toChannelCfg / toCameraConfig 转换
#   5) LiveCfg::whipUrl URL 模板替换
#   6) osdCfgChanged 变更检测
# ---------------------------------------------------------------------------

include(GoogleTest)

set(_mediad_config_includes
    ${CMAKE_SOURCE_DIR}/apps/mediad/src
    ${CMAKE_SOURCE_DIR}/apps/mediad/src/config
    ${CMAKE_SOURCE_DIR}/libs/camera/include   # camera/types.h, camera/camera_config.h
)

# --- test_config_store ---
add_executable(test_config_store
    test_config_store.cpp
    ${CMAKE_SOURCE_DIR}/apps/mediad/src/config/ConfigStore.cpp
    ${CMAKE_SOURCE_DIR}/libs/camera/src/camera_config_impl.cpp   # Builder::build() 无 SDK 依赖
)
target_include_directories(test_config_store PRIVATE ${_mediad_config_includes})
target_link_libraries(test_config_store
    PRIVATE nc::cjson nc::common nc::compile_options
            GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_config_store PROPERTIES TIMEOUT 30)
