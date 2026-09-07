# ---------------------------------------------------------------------------
# nc::http 单元测试 (tests/libs/http/)
#
# 测什么:
#   1) downloadWithPolicy (高层下载策略) —— fake HTTP server 模拟成功/失败/重试/取消
#   2) DownloadPolicy::checkDiskSpace   —— 磁盘空间检查 (statvfs 包装)
#   3) DownloadPolicy 默认值            —— 防止默认值漂移破坏调用方
#
# 拆成多个 executable, 每个测试 .cpp 一个独立可执行:
#   - 用户可单独跑任一文件 (./test_download --gtest_filter=...)
#   - 编译按需: 改一个 .cpp 只重编一个 exe, 不全量
#   - 错误定位更准: 编译失败只看那一个 exe 的 stderr
#
# 拆分的成本: CMakeLists 多几行 target_link_libraries, 但 GTest/Microhttpd 已在
# 顶层 tests/CMakeLists.txt find 过了, 不会重复
# ---------------------------------------------------------------------------

include(GoogleTest)

# --- test_download: 端到端下载/重试/取消 (要 fake server) ---
add_executable(test_download
    test_download.cpp
)

target_link_libraries(test_download
    PRIVATE
        nc::http                # downloadWithPolicy (高层) + HttpClient (底层)
        nc::common              # NC_LOGx
        nc::compile_options     # -Wall -Wextra cxx_std_17
        GTest::gtest
        GTest::gtest_main
        nc_test_fake_http_server::nc_test_fake_http_server
)

# 单测最多 60 秒 (NotFound 重试等待可能要 15s, 留足余量)
gtest_discover_tests(test_download PROPERTIES TIMEOUT 60)

# --- test_check_disk_space: DownloadPolicy::checkDiskSpace + 默认值 (纯逻辑, 无 fake server) ---
add_executable(test_check_disk_space
    test_check_disk_space.cpp
)

target_link_libraries(test_check_disk_space
    PRIVATE
        nc::http                # DownloadPolicy::checkDiskSpace
        nc::common
        nc::compile_options
        GTest::gtest
        GTest::gtest_main
)

gtest_discover_tests(test_check_disk_space PROPERTIES TIMEOUT 30)

# --- test_http_client: HttpClient 上传/下载/进度回调/错误路径 (要 fake server) ---
add_executable(test_http_client
    test_http_client.cpp
)

target_link_libraries(test_http_client
    PRIVATE
        nc::http                # HttpClient (底层)
        nc::common              # NC_LOGx
        nc::compile_options     # -Wall -Wextra cxx_std_17
        GTest::gtest
        GTest::gtest_main
        nc_test_fake_http_server::nc_test_fake_http_server
)

# 单测最多 30 秒 (下载测试不需要重试等待)
gtest_discover_tests(test_http_client PROPERTIES TIMEOUT 30)
