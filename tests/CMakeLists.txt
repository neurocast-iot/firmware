# ---------------------------------------------------------------------------
# NeuroCast 测试集顶层入口
#
# 门控：只有 NC_BUILD_TESTS=ON（x86-debug preset 默认开）时才编。
# ARM 板端预设不编测试, 避免 GoogleTest 拖慢 ARM 编译。
#
# 平台：x86 host only。板端测试如果将来要加, 单独搞一个 board-tests/。
#
# 测试框架：GoogleTest 1.14.0 (最后一个 C++14 兼容版, 对齐 ARM 工具链)
# Fake server：libmicrohttpd 1.0.0 (起本地 HTTP server 测下载逻辑)
#
# 目录结构 (集中式, 镜像代码层)：
#   test_support/  跨测试共享工具 (fake_http_server 等)
#   libs/          库单元测试, 镜像 libs/ 树 (每个子目录对应一个库)
#   apps/          进程集成测试, 镜像 apps/ 树 (每个子目录对应一个进程)
#
# 强约束: 不准在 libs/<lib_name>/ 下面建 tests/ 子目录, 必须把测试放这里。
# 违反这条 = 失去集中式收益 (跨模块测试找不到放哪, 工具链配置散落各处)。
# ---------------------------------------------------------------------------
if(NOT NC_BUILD_TESTS)
    message(STATUS "tests/ skipped (NC_BUILD_TESTS=OFF)")
    return()
endif()

# --- GTest + libmicrohttpd：所有测试子目录共享, 在这里统一 find 一次 ---
# 沙箱环境连不上 github.com, FetchContent 走不通; 退而求其次用系统包 (libgtest-dev + libmicrohttpd-dev)
find_package(GTest REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MICROHTTPD REQUIRED libmicrohttpd)

# --- 子目录分发 ---
add_subdirectory(test_support)
add_subdirectory(libs)
add_subdirectory(apps)
