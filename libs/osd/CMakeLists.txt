# ---------------------------------------------------------------------------
# nc::osd —— OSD 水印库（自 gw_av100/application/lib-osd-cpp 保真迁入）
# 原 target 名 osd_cpp → nc::osd；配方照原 CMakeLists：C++14、
# -fno-rtti -fno-exceptions -Os、符号隐藏
#
# 平台无关化已完成：硬件调用全部下沉 pal::IOsdBackend
#   （硬件后端 / linux-generic 软渲染后端），本库不再引任何厂商头，
#   各平台均可编译，门控已解除（libs/CMakeLists.txt）
# ---------------------------------------------------------------------------
add_library(nc_osd STATIC
    src/TimeElement.cpp
    src/TextElement.cpp
    src/RectElement.cpp
    src/CircleElement.cpp
    src/EllipseElement.cpp
    src/PolygonElement.cpp   # 多边形（扫描线栅格化后走 DrawBitmap）
    src/BitmapElement.cpp    # 位图（BMP 解析 + 就近色量化）
    src/Utf8ToGb2312.cpp     # UTF-8→GB2312 转换（内嵌查表，设备无 iconv）
    src/OsdEngine.cpp   # OSD 引擎（实例化门面，持有平台后端）
)
add_library(nc::osd ALIAS nc_osd)

# 原工程为 C++14；符号隐藏为原配方（嵌入式缩减导出表）
set_target_properties(nc_osd PROPERTIES
    CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(nc_osd
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include       # 消费者 include "osd/OsdEngine.h"
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/osd   # 源文件保持原相对包含，零改动
)

# 原库编译选项（-fno-rtti -fno-exceptions -Os，嵌入式约束），沿用避免行为差异
target_compile_options(nc_osd PRIVATE -fno-rtti -fno-exceptions -Os)

# 平台差异全部归 pal 后端：本库只依赖接口头（nc::pal 为 INTERFACE，只带 include 路径）；
# 具体后端实现（nc::pal_backend）由最终可执行文件链接，静态库不锁死平台
target_link_libraries(nc_osd PUBLIC nc::pal)

# 冒烟测试仅宿主平台可跑（依赖 linux-generic 软渲染后端；硬件后端不进单测）
if(NC_BUILD_TESTS AND NC_PLATFORM STREQUAL "linux-generic")
    add_subdirectory(tests)
endif()
