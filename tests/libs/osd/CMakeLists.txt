# ---------------------------------------------------------------------------
# osd:: 单元测试 (tests/libs/osd/)
#
# 测什么:
#   1) matchColorIndex      —— 颜色匹配算法（精确 + 最邻近）
#   2) calculateFontSize    —— 视频宽度 → 字体大小映射
#   3) utf8WidthUnits       —— UTF-8 字符串宽度计算
#   4) calculateTextWidth   —— 文本画布宽度
#   5) clampToBounds        —— 画布边界限制
#   6) calculateStride      —— 步长对齐
#
# 纯逻辑，无硬件依赖，x86 直接跑
# ---------------------------------------------------------------------------

include(GoogleTest)

# --- test_osd_utils: OSD 工具函数（纯逻辑） ---
add_executable(test_osd_utils
    test_osd_utils.cpp
)

target_link_libraries(test_osd_utils
    PRIVATE
        nc::osd               # OSD 工具函数
        nc::common            # NC_LOGx
        nc::compile_options   # -Wall -Wextra cxx_std_17
        GTest::gtest
        GTest::gtest_main
)

# 单测最多 10 秒（纯逻辑，很快）
gtest_discover_tests(test_osd_utils PROPERTIES TIMEOUT 10)
