# ---------------------------------------------------------------------------
# nc::config_sync —— 配置同步框架
#
# 职责：
#   - 提供通用的配置同步机制，保证 iot_agent 与各服务之间的配置一致性
#   - 服务侧：ConfigSyncClient，处理启动通知、配置接收、ACK 回复
#   - iot_agent 侧：ConfigSyncManager，管理服务状态、配置下发、重试
#
# 依赖：
#   nc::mq       IPC 消息传输
#   nc::common   日志工具
#   nc::cjson    JSON 解析
# ---------------------------------------------------------------------------

add_library(nc_config_sync
    src/config_sync_types.cpp
    src/config_sync_client.cpp
    src/config_sync_manager.cpp
)

add_library(nc::config_sync ALIAS nc_config_sync)

target_include_directories(nc_config_sync PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nc_config_sync
    PUBLIC
        nc::mq
    PRIVATE
        nc::common
        nc::cjson
        nc::compile_options
)
