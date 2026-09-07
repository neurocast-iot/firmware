# ---------------------------------------------------------------------------
# libs/sensor：硬件传感器共享库
#
# 串口底层工具 + 各传感器 Reader（Zigbee / GPS / 蓝牙 ...）
# 只依赖 POSIX 串口操作，不依赖厂商 SDK，所有平台都能编译。
# ---------------------------------------------------------------------------
add_library(nc_sensor STATIC
    src/serial_port.cpp
    src/zigbee_reader.cpp
    src/bt_frame_parser.cpp
    src/bt_tag_reader.cpp
)

target_include_directories(nc_sensor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nc_sensor PUBLIC nc_common PRIVATE nc::compile_options)

add_library(nc::sensor ALIAS nc_sensor)
