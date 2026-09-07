# ---------------------------------------------------------------------------
# iot_agent —— IoT 平台对接代理（编译期裁剪 + 多平台选一）
#
# 职责：
#   - 对接 MQTT 平台（首版 ThingsBoard，后续可加 EMQX / 阿里云 IoT 等）
#   - 获取凭据（TB 走 provision；EMQX 走业务后端签发；等等）
#   - 用凭据登录、订阅平台主题、接收配置 / RPC 指令
#   - 通过 IPC 把配置 / RPC 分发给 mediad / ota_agent 等业务进程
#
# 架构：
#   平台无关层：AgentConfig / ConfigRouter / RpcRouter / DeviceData
#   平台适配层：CloudAdapter 接口 + 工厂 + 各平台实现（TBAdapter 等）
#   底层 MQTT：复用 libs/mqtt（nc::mqtt::MqttClient，Paho 异步封装）
#   main.cpp 只做组装（创建 + 连线 + 启动），业务逻辑全在各模块内部
#
# 编译期裁剪：
#   set(IOT_AGENT_PLATFORM "thingsboard")  → 只编 TB 代码
#   set(IOT_AGENT_PLATFORM "aws")          → 只编 AWS 代码
#   不选的平台代码完全不进二进制
#
# 依赖：
#   nc::mqtt_client  Paho MQTT 客户端
#   nc::mq           ZeroMQ IPC（跨进程分发给 mediad）
#   nc::common       日志 / 文件工具
#   nc::cjson        JSON 解析
#   nc::curl         libcurl HTTP 客户端（文件上传服务）
#   nc::openssl      OpenSSL 静态库（OTA SHA256 校验）
# ---------------------------------------------------------------------------

# ---- 平台选择（编译期裁剪） ----
set(IOT_AGENT_PLATFORM "thingsboard" CACHE STRING "IoT cloud platform: thingsboard / aws / emqx")

# OTA 模块源文件（云端推送升级 + 本地信号升级，全部内嵌在 iot_agent 进程里）
# 通用下载能力走 nc::http 库的 downloadWithPolicy 自由函数（HttpClient + DownloadPolicy 组合），
# iot_agent 自己只保留业务流（OtaManager / SwHandler / FwHandler / sha256）
set(OTA_SOURCES
    src/ota/ota_manager.cpp
    src/ota/ota_result_reporter.cpp
    src/ota/local_upgrade_trigger.cpp
    src/ota/backup_manager.cpp
    src/ota/upgrade_handler_base.cpp
    src/ota/verifier/manifest_parser.cpp
)

# 根据平台选源文件 + 定义编译宏
if(IOT_AGENT_PLATFORM STREQUAL "thingsboard")
    set(PLATFORM_SOURCES
        src/cloud/thingsboard/tb_adapter.cpp
        src/cloud/thingsboard/tb_provision.cpp
        src/cloud/thingsboard/token_store.cpp
    )
    set(PLATFORM_DEFINES PLATFORM_THINGSBOARD)
    message(STATUS "iot_agent: platform = ThingsBoard")

elseif(IOT_AGENT_PLATFORM STREQUAL "aws")
    set(PLATFORM_SOURCES
        # src/cloud/aws/aws_adapter.cpp
    )
    set(PLATFORM_DEFINES PLATFORM_AWS)
    message(STATUS "iot_agent: platform = AWS IoT")

elseif(IOT_AGENT_PLATFORM STREQUAL "emqx")
    set(PLATFORM_SOURCES
        # src/cloud/emqx/emqx_adapter.cpp
    )
    set(PLATFORM_DEFINES PLATFORM_EMQX)
    message(STATUS "iot_agent: platform = EMQX")

else()
    message(FATAL_ERROR "iot_agent: unsupported IOT_AGENT_PLATFORM=${IOT_AGENT_PLATFORM}")
endif()

# ---- 目标 ----
add_executable(iot_agent
    src/main.cpp
    src/App.cpp
    src/config/agent_config.cpp
    src/config/cloud_config_translator.cpp
    src/cloud/platform_factory.cpp
    src/cloud/cloud_service.cpp
    src/device/device_service.cpp
    src/router/config_router.cpp
    src/router/rpc_handler.cpp
    src/ipc/ipc_hub.cpp
    src/ipc/ipc_event_handler.cpp
    src/ipc/signal_manager.cpp
    src/upload/file_upload_client.cpp
    src/upload/file_upload_service.cpp
    src/upload/upload_result_reporter.cpp
    src/router/attribute_handler.cpp
    ${OTA_SOURCES}
    ${PLATFORM_SOURCES}
)

# 平台编译宏
target_compile_definitions(iot_agent PRIVATE ${PLATFORM_DEFINES})

target_include_directories(iot_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cloud
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/upload
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ota
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ota/verifier
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ota/sw
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ota/fw
)

# TB 特有头文件路径（只有选 TB 时才加）
if(IOT_AGENT_PLATFORM STREQUAL "thingsboard")
    target_include_directories(iot_agent PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cloud/thingsboard
    )
endif()

target_link_libraries(iot_agent
    PRIVATE
        nc::mqtt_client
        nc::mq
        nc::config_sync
        nc::common
        nc::cjson
        nc::sensor
        nc::compile_options
        nc::http      # HTTP 工具包 (含 downloadWithPolicy 自由函数)
        nc::curl
        nc::openssl   # OTA 模块 SHA256 校验
)
