# test_p2p_pusher：P2P 推流测试（设备端 answer 方，metaRTC 8.0）
# 信令走 MQTT 同步 API（nc::mqtt_c），媒体走 nc::metartc；
# 自 gw_av100/application/test-p2p-rtc8 迁入，源码未改动；
# viewer/ 与 deploy/ 为配套观看页与信令 broker 编排，随目录入库不参与编译
add_executable(test_p2p_pusher
    test_p2p_pusher.cpp
    P2PStreamer.cpp
    P2PSignaling.cpp
)

# 原工程为 C++11
set_target_properties(test_p2p_pusher PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)

target_link_libraries(test_p2p_pusher PRIVATE
    nc::camera
    nc::metartc     # 四个 metaRTC 静态库 + openssl/srtp2/usrsctp（third_party/deps.cmake 解析）
    nc::mqtt_c      # libpaho-mqtt3c.a 静态链入，替代原 rootfs 动态 .so
    nc::cjson
    pthread
    dl
    m
)
