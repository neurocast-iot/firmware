# ---------------------------------------------------------------------------
# mediactl —— mediad 调试 CLI 客户端（curl 式：发 IPC 命令/订阅事件）
# 只依赖 nc::mq + nc::cjson，无厂商 SDK 依赖，host/板端双平台可构建
# （host 上用于 CI 编译验证，板上用于现场排障）
# ---------------------------------------------------------------------------
add_executable(mediactl
    src/main.cpp
)

set_target_properties(mediactl PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# 协议头单一来源：以 mediad/src 为 include 根引用 "ipc/MediadMessages.h"（纯常量定义，无 SDK 依赖）
target_include_directories(mediactl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../mediad/src)

target_link_libraries(mediactl PRIVATE
    nc::common
    nc::mq
    nc::cjson
    pthread
)
