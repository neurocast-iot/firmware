# ---------------------------------------------------------------------------
# pal backend：mock（单元测试桩）
# 与 nc::pal_backend 并存：测试代码直接链接 nc::pal_mock 中的 Mock* 类，
# 不提供 Create* 工厂符号，避免与当前平台 backend 冲突
# ---------------------------------------------------------------------------
add_library(nc_pal_mock STATIC
    mock_backends.cpp
)
add_library(nc::pal_mock ALIAS nc_pal_mock)

target_include_directories(nc_pal_mock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(nc_pal_mock
    PUBLIC nc::pal
    PRIVATE nc::compile_options
)
