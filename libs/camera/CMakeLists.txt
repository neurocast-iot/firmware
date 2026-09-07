# ---------------------------------------------------------------------------
# nc::camera —— 摄像头驱动抽象层（媒体域库，各平台驱动收在 src/<平台>/ 下）
#
# 架构：
#   CameraDevice (公共接口) → CameraDriver (抽象) → src/<平台>/ 具体实现
#   驱动实例由 src/<平台>/driver_factory.cpp 按编译期平台创建，
#   编排层（camera_device_impl.cpp）不包含任何厂商头文件。
#
# 编译期选择：跟随全局 NC_PLATFORM（与 libs/CMakeLists.txt 门控同源），
#   新增平台 = 新写 src/<平台>/ 目录 + 下面加一个分支，公共代码零改动。
# ---------------------------------------------------------------------------

if(NOT DEFINED NC_PLATFORM)
    message(FATAL_ERROR "nc::camera requires NC_PLATFORM (set by top-level CMake/preset)")
endif()

message(STATUS "nc::camera platform: ${NC_PLATFORM}")

# 公共源文件（平台无关）
set(CAMERA_COMMON_SRCS
    src/frame_impl.cpp
    src/event_dispatcher_impl.cpp
    src/camera_config_impl.cpp
    src/camera_device_impl.cpp
)

# 按平台选择驱动实现（厂商头文件/库只出现在这些源文件和下面对应分支里）
if(NC_PLATFORM STREQUAL "anyka-av100")
    set(CAMERA_VENDOR_SRCS
        src/anyka/driver_factory.cpp
        src/anyka/anyka_channel_impl.cpp
        src/anyka/anyka_encoder_impl.cpp
        src/anyka/anyka_device_impl.cpp
        src/anyka/jpeg_encoder.cpp
        src/anyka/motion_detector.cpp
    )
    # Anyka SDK 链接库（符号沿板端 /usr/lib 的 .so 解析）
    set(CAMERA_VENDOR_SDK_LIBS
        app_video plat_vi plat_vpss plat_isp_sdk mpi_venc mpi_mux mpi_demux
        akmedia plat_common plat_mem plat_log plat_thread plat_osal plat_dbg plat_timer
        ak_mrd
    )
else()
    message(FATAL_ERROR "nc::camera: no driver implementation for platform '${NC_PLATFORM}'")
endif()

add_library(nc_camera STATIC
    ${CAMERA_COMMON_SRCS}
    ${CAMERA_VENDOR_SRCS}
)
add_library(nc::camera ALIAS nc_camera)

# 原工程为 C++14（GCC 5.5 支持），沿用避免行为差异
set_target_properties(nc_camera PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

target_include_directories(nc_camera
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include     # 消费者直接 include camera/
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src         # 内部使用 driver_factory.h 与平台适配层
)

# 原库编译选项（-fno-rtti -Os，嵌入式约束），沿用避免行为差异
target_compile_options(nc_camera PRIVATE -fno-rtti -Os -Wall -Wextra -Wno-unused-parameter)

# 厂商 SDK 头文件/库目录（仅平台分支需要；目录也收进平台分支，避免别的平台误用）
if(NC_VENDOR_SDK_ROOT)
    target_include_directories(nc_camera PRIVATE ${NC_VENDOR_SDK_ROOT}/include)
    target_link_directories(nc_camera PUBLIC ${NC_VENDOR_SDK_ROOT}/lib)
else()
    message(WARNING "NC_VENDOR_SDK_ROOT not set: nc::camera will fail to compile SDK calls")
endif()

# SDK 库以链接名传播给最终可执行文件（消费者无需重复声明）
target_link_libraries(nc_camera
    PUBLIC
        ${CAMERA_VENDOR_SDK_LIBS}
    PRIVATE
        pthread
)
