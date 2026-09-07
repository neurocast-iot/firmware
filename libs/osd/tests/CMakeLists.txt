# ---------------------------------------------------------------------------
# nc::osd 冒烟测试（仅宿主平台）：元素生命周期走软渲染后端，断言后端调用计数
# 门控在上层（libs/osd/CMakeLists.txt）：NC_BUILD_TESTS 且 NC_PLATFORM=linux-generic
# ---------------------------------------------------------------------------
add_executable(osd_engine_smoke_test osd_engine_smoke_test.cpp)

target_link_libraries(osd_engine_smoke_test PRIVATE
    nc::osd
    nc::pal_backend
)

# 软后端内省头在后端实现目录，不进公共 include 路径（生产代码看不到它）
target_include_directories(osd_engine_smoke_test PRIVATE
    ${CMAKE_SOURCE_DIR}/pal/backends/linux-generic
)

add_test(NAME osd_engine_smoke_test COMMAND osd_engine_smoke_test)
