# ---------------------------------------------------------------------------
# pal：平台抽象层
#   nc::pal          纯接口（INTERFACE，只有头文件）
#   nc::pal_backend  当前平台的实现（由 NC_PLATFORM 选择，恰好一个）
#   nc::pal_mock     单元测试桩（仅 NC_BUILD_TESTS 时构建）
# ---------------------------------------------------------------------------
add_library(nc_pal INTERFACE)
add_library(nc::pal ALIAS nc_pal)
target_include_directories(nc_pal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nc_pal INTERFACE nc::compile_options)

# 按 NC_PLATFORM 选择唯一 backend（开闭原则：新平台只需新增目录 + 分支）
if(NC_PLATFORM STREQUAL "linux-generic")
    add_subdirectory(backends/linux-generic)
else()
    message(FATAL_ERROR "Unknown NC_PLATFORM: ${NC_PLATFORM}")
endif()

if(NC_BUILD_TESTS)
    add_subdirectory(backends/mock)
endif()
