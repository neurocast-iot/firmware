# ---------------------------------------------------------------------------
# nc::recorder —— MP4 录像/解封装库（平台驱动封装）
#
# 多平台架构：
#   - 公共层（mp4_recorder.cpp / mp4_demuxer.cpp）只管业务逻辑
#   - SDK 调用全部下沉到 src/<平台>/*_driver.cpp 平台实现
#   - 新增平台 = 新写 src/<平台>/driver_factory.cpp，公共层零改动
#
# 当前支持：anyka-av100（src/anyka/）；开源版不含厂商 SDK 后端，
# 贡献者新增平台 = 新写 src/<平台>/driver_factory.cpp，公共层零改动
# ---------------------------------------------------------------------------
add_library(nc_recorder STATIC
    src/mp4_recorder.cpp
    src/mp4_demuxer.cpp
)
add_library(nc::recorder ALIAS nc_recorder)

# 与 camera/osd 同龄工具链（GCC 5.5），统一 C++14
set_target_properties(nc_recorder PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

target_include_directories(nc_recorder
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include      # 消费者 include "recorder/mp4_recorder.h"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src         # 内部 driver_factory.h
)

target_compile_options(nc_recorder PRIVATE -Os -Wall -Wextra)

target_link_libraries(nc_recorder
    PRIVATE nc::common      # 日志/目录工具
)

# 平台驱动实现（按 NC_PLATFORM 选择编译）
if(NC_PLATFORM STREQUAL "anyka-av100")
    target_sources(nc_recorder PRIVATE
        src/anyka/driver_factory.cpp
        src/anyka/anyka_recorder_driver.cpp
        src/anyka/anyka_demuxer_driver.cpp
    )
    # 厂商 SDK 头文件/库（mux 符号在 mpi_mux、demux 符号在 mpi_demux）
    if(NC_VENDOR_SDK_ROOT)
        target_include_directories(nc_recorder PRIVATE ${NC_VENDOR_SDK_ROOT}/include)
        target_link_directories(nc_recorder PUBLIC ${NC_VENDOR_SDK_ROOT}/lib)
        target_link_libraries(nc_recorder PUBLIC mpi_mux mpi_demux)
    else()
        message(WARNING "NC_VENDOR_SDK_ROOT not set: nc::recorder will fail to compile SDK calls")
    endif()
else()
    message(WARNING "nc::recorder: no platform backend for NC_PLATFORM=${NC_PLATFORM}")
endif()
