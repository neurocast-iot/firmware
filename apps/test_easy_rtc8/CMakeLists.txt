# ---------------------------------------------------------------------------
# test_easy_rtc8 —— WebRTC 推流集成测试（自 gw_av100/application/test-easy-rtc8 复制，源码未改动）
# CameraDevice 编码回调 → WebRTCStreamer(YangPeerConnection8) → WHIP 推流
# 仅板端平台构建；main.cpp 为低层级参考实现，不参与编译（与原工程一致）
# ---------------------------------------------------------------------------
add_executable(test_webrtc_streamer8
    test_webrtc_streamer8.cpp
    WebRTCStreamer.cpp
)

# 原工程为 C++11
set_target_properties(test_webrtc_streamer8 PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)

target_link_libraries(test_webrtc_streamer8 PRIVATE
    nc::camera
    nc::metartc     # 四个 metaRTC 静态库 + openssl/srtp2/usrsctp（third_party/deps.cmake 解析）
    pthread
    dl
    m
)
