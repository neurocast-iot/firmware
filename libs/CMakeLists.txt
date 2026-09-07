# ---------------------------------------------------------------------------
# libs：通用库层（只依赖 pal 接口和彼此，禁止直接接触厂商 SDK）
# ---------------------------------------------------------------------------
add_subdirectory(common)
add_subdirectory(sensor)
add_subdirectory(mq)
add_subdirectory(config_sync)
add_subdirectory(exporter)
add_subdirectory(osd)       # OSD 水印库（平台无关，各平台均编译）
add_subdirectory(http)      # HTTP 工具包（基于 libcurl），含 HttpClient + 高层下载策略

# 以下库依赖硬件相关库或板端专用依赖，按平台门控编译：
#   camera / recorder  —— 需要平台驱动实现（src/<platform>/）
#   rtc / mqtt_client  —— 依赖 metaRTC / Paho MQTT（板端专用配方）
# 贡献者新增平台时，在此添加对应的 if 分支并实现 src/<platform>/ 驱动
if(NC_PLATFORM STREQUAL "anyka-av100")
    add_subdirectory(camera)
    add_subdirectory(recorder)
    add_subdirectory(rtc)
    add_subdirectory(mqtt)
endif()
