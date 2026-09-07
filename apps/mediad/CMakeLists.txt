# ---------------------------------------------------------------------------
# mediad —— 设备媒体能力进程（相机/拍照/录像/OSD 水印，未来推流/P2P）
# 命令总线架构：IpcClient(DEALER) → CommandRouter → 各能力服务
# 仅板端平台构建（依赖 nc::camera/nc::osd/nc::recorder）
#
# src/ 按职责分层（对齐 iot_live-cpp 的目录组织）：
#   config/    配置中心（加载/热更/落盘）
#   ipc/       对外协议与命令总线（消息定义/路由/IPC 服务端）
#   services/  能力服务（相机/OSD/拍照/录像）
#   triggers/  触发源管理（定时/蓝牙/SOS 等触发源 + 时间表）
#
# 可选功能：
#   MEDIAD_BT_SUPPORT —— 蓝牙触发源（依赖 nc::sensor，增大约 100KB）
#   嵌入式设备内存紧张时默认关闭，需要蓝牙时 cmake -DMEDIAD_BT_SUPPORT=ON
# ---------------------------------------------------------------------------

option(MEDIAD_BT_SUPPORT "启用蓝牙触发源（依赖 nc::sensor）" OFF)

set(MEDIAD_SRCS
    src/main.cpp
    src/App.cpp
    src/config/ConfigStore.cpp
    src/services/CameraService.cpp
    src/services/OsdService.cpp
    src/services/SnapshotService.cpp
    src/services/RecordService.cpp
    src/services/LiveStreamService.cpp
    src/ipc/CommandRouter.cpp
    src/ipc/IpcClient.cpp
    src/triggers/schedule.cpp
    src/triggers/timer_trigger.cpp
    src/triggers/record_trigger.cpp
    src/triggers/trigger_manager.cpp
)

# 蓝牙触发源：编译开关控制，默认关闭（嵌入式设备内存紧张）
if(MEDIAD_BT_SUPPORT)
    list(APPEND MEDIAD_SRCS src/triggers/bluetooth_trigger.cpp)
    message(STATUS "mediad: bluetooth trigger support ENABLED")
else()
    message(STATUS "mediad: bluetooth trigger support DISABLED (use -DMEDIAD_BT_SUPPORT=ON to enable)")
endif()

add_executable(mediad ${MEDIAD_SRCS})

# 仓内 include 统一以 src/ 为根的子目录限定路径（如 "ipc/MediadMessages.h"）
target_include_directories(mediad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(mediad PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# 链接器标志汇总
set(_mediad_link_flags "-Wl,--allow-multiple-definition")  # openssl 和 paho-mqtt 的 SHA1 符号冲突，允许覆盖（功能等价）

# 发布版自动 strip，去掉调试符号省内存（嵌入式设备 64MB RAM 紧张）
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    string(APPEND _mediad_link_flags " -s")  # 链接时 strip，等价于手动跑 strip
endif()

set_target_properties(mediad PROPERTIES LINK_FLAGS "${_mediad_link_flags}")

target_link_libraries(mediad PRIVATE
    nc::camera
    nc::osd
    nc::pal_backend   # OSD 平台后端（nc::osd 只依赖接口，具体实现由可执行文件链入）
    nc::recorder
    nc::rtc
    nc::mq
    nc::config_sync
    nc::cjson
    nc::common
    pthread
)

# 蓝牙触发源需要 nc::sensor（串口通信 + 帧解析）
if(MEDIAD_BT_SUPPORT)
    target_link_libraries(mediad PRIVATE nc::sensor)
    target_compile_definitions(mediad PRIVATE MEDIAD_BT_SUPPORT=1)
endif()
