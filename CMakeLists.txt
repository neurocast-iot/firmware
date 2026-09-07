cmake_minimum_required(VERSION 3.20)

project(neurocast
    VERSION 0.1.0
    DESCRIPTION "NeuroCast multi-platform embedded application monorepo"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# 全局选项
# ---------------------------------------------------------------------------
# NC_PLATFORM 由 CMakePresets 绑定，决定编译哪个 pal backend
# 可选值：linux-generic（开源默认）| 自定义平台（贡献者新增）
set(NC_PLATFORM "linux-generic" CACHE STRING "Target platform (selects pal backend)")
set_property(CACHE NC_PLATFORM PROPERTY STRINGS linux-generic)

option(NC_BUILD_TESTS "Build unit tests (mock backend + ctest)" OFF)

message(STATUS "NeuroCast platform: ${NC_PLATFORM}")

# ---------------------------------------------------------------------------
# 公共编译选项 / 模块路径
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
include(NcCompileOptions)

if(NC_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# 第三方依赖（FetchContent 声明，源码不入库）
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/third_party/deps.cmake)

# ---------------------------------------------------------------------------
# 模块编排（依赖方向严格单向：apps → libs → pal 接口 ← backends）
# ---------------------------------------------------------------------------
add_subdirectory(pal)
add_subdirectory(libs)
add_subdirectory(apps)

# 测试集（仅 NC_BUILD_TESTS=ON 才进入，与 enable_testing() 门控一致）
if(NC_BUILD_TESTS)
    add_subdirectory(tests)
endif()
