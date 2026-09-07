# ---------------------------------------------------------------------------
# nc::common 单元测试 (tests/libs/common/)
#
# 测什么:
#   1) string_utils  —— TrimString + ExtractFileNameFromUrl (字符串工具)
#   2) file_utils    —— FileExists/ReadFile/WriteFileAtomic/CopyFile/MakeDirs/ExtractFileName
#   3) crypto_utils  —— CalculateFileSHA256 (文件哈希计算)
#
# 拆成多个 executable, 每个测试 .cpp 一个独立可执行:
#   - 用户可单独跑任一文件 (./test_string_utils --gtest_filter=...)
#   - 编译按需: 改一个 .cpp 只重编一个 exe, 不全量
#   - 错误定位更准: 编译失败只看那一个 exe 的 stderr
# ---------------------------------------------------------------------------

include(GoogleTest)

# --- test_string_utils: 字符串工具 (纯逻辑, 无文件操作) ---
add_executable(test_string_utils
    test_string_utils.cpp
)

target_link_libraries(test_string_utils
    PRIVATE
        nc::common              # nc::common::TrimString + ExtractFileNameFromUrl
        nc::compile_options     # -Wall -Wextra cxx_std_17
        GTest::gtest
        GTest::gtest_main
)

gtest_discover_tests(test_string_utils PROPERTIES TIMEOUT 10)

# --- test_file_utils: 文件工具 (用临时目录做文件操作) ---
add_executable(test_file_utils
    test_file_utils.cpp
)

target_link_libraries(test_file_utils
    PRIVATE
        nc::common              # nc::common::FileExists/ReadFile/WriteFileAtomic/CopyFile/MakeDirs/ExtractFileName
        nc::compile_options
        GTest::gtest
        GTest::gtest_main
)

# 文件操作测试最多 30 秒（创建/删除临时文件可能要几秒）
gtest_discover_tests(test_file_utils PROPERTIES TIMEOUT 30)

# --- test_crypto_utils: 哈希工具 (用临时文件算 SHA256) ---
add_executable(test_crypto_utils
    test_crypto_utils.cpp
)

target_link_libraries(test_crypto_utils
    PRIVATE
        nc::common              # nc::common::CalculateFileSHA256 + WriteFileAtomic + MakeDirs
        nc::compile_options
        GTest::gtest
        GTest::gtest_main
)

# SHA256 计算测试最多 30 秒（1MB 文件算哈希可能要几秒）
gtest_discover_tests(test_crypto_utils PROPERTIES TIMEOUT 30)
