# ---------------------------------------------------------------------------
# nc::rtc —— 实时视频库（metaRTC 8.0 封装：P2P 会话 / WHIP 推流 / MQTT 信令）
# 自 apps/test_p2p_rtc8 与 apps/test_easy_rtc8 两个已验证测试工程"中和"抽取：
#   frame_sender   共用帧管线（两工程逐行一致的帧队列/发送线程/Pacer 喂帧）
#   p2p_session    P2P 连接层（answer 方 Full-ICE，源自 P2PStreamer）
#   whip_pusher    WHIP 推 SRS 连接层（源自 WebRTCStreamer）
#   p2p_signaling  MQTT 信令（迁入 + relay/alive 两条消息）
#
# 说明：metaRTC/paho 均为 third_party 静态库（deps.cmake 解析），不接触厂商
#   SDK，但消费方 mediad 仅板端存在，与其同门控平台编译
#   （libs/CMakeLists.txt 门控），避免 x86 平台空耗构建时间
# ---------------------------------------------------------------------------
add_library(nc_rtc STATIC
    src/ice_config.cpp
    src/frame_sender.cpp
    src/p2p_session.cpp
    src/whip_pusher.cpp
    src/p2p_signaling.cpp
)
add_library(nc::rtc ALIAS nc_rtc)

# 与 camera/recorder 同龄工具链（GCC 5.5），统一 C++14
set_target_properties(nc_rtc PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

target_include_directories(nc_rtc
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include      # 消费者 include "rtc/p2p_session.h"
)

target_compile_options(nc_rtc PRIVATE -Os -Wall -Wextra)

target_link_libraries(nc_rtc
    PUBLIC
        nc::metartc     # 公共头暴露 YangPeerConnection8（include 路径需传播）
        nc::mqtt_c      # 公共头暴露 MQTTClient.h（include 路径需传播）
    PRIVATE
        nc::cjson       # 信令 JSON 封装（仅实现内使用）
        nc::common      # NC_LOGx 日志门面
        pthread
)
