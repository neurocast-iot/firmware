# ---------------------------------------------------------------------------
# nc::mqtt_client —— 自愈型 MQTT 客户端（Paho MQTTAsync 封装）
# 迁自 gw_av100/application/iot_live（已上线验证）：自动重连 / 消息缓冲 /
# 订阅恢复 / 防扎堆 / 独立回调派发线程，面向 IoT 业务通道（设备上报、指令下发）
#
# 说明：底座 nc::mqtt（libpaho-mqtt3as 异步+SSL，deps.cmake 聚合）依赖
#   openssl-fit（ARM 配方），与 rtc 同门控平台编译
#   （libs/CMakeLists.txt 门控）；P2P 信令走 nc::rtc 的薄封装，不用本库
# ---------------------------------------------------------------------------
add_library(nc_mqtt_client STATIC
    src/mqtt_client.cpp
)
add_library(nc::mqtt_client ALIAS nc_mqtt_client)

# 与 camera/rtc 同龄工具链（GCC 5.5），统一 C++14
set_target_properties(nc_mqtt_client PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

target_include_directories(nc_mqtt_client
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include      # 消费者 include "nc/mqtt/mqtt_client.h"
)

target_compile_options(nc_mqtt_client PRIVATE -Os -Wall -Wextra)

target_link_libraries(nc_mqtt_client
    PUBLIC
        nc::mqtt        # 公共头暴露 MQTTAsync.h（include 路径需传播）
    PRIVATE
        nc::common      # NC_LOGx 日志门面
        pthread
)
