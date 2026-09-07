# apps：应用层（只依赖 libs，不直接接触厂商 SDK）
#
# 注：原 ota_agent 独立进程已废除（与 iot_agent 重复持有 MQTT 连接 + 进程
# 隔离收益小），整套 OTA 逻辑迁入 iot_agent/ota/ 子模块
#
# 门控说明：
#   mediad / iot_agent 依赖硬件相关库（nc::camera/nc::recorder/nc::rtc）
#   或板端专用依赖（nc::mqtt_client），仅在对应平台可用时构建。
#   贡献者新增平台时，在此添加对应的 if 分支即可。
# ---------------------------------------------------------------------------
add_subdirectory(iot_monitor)

# 依赖硬件媒体库的应用（需要 nc::camera/nc::recorder/nc::rtc 等平台驱动）
# 新增平台时在此添加对应的 NC_PLATFORM 分支
if(NC_PLATFORM STREQUAL "anyka-av100")
    add_subdirectory(mediad)        # 媒体能力进程（依赖 nc::camera/nc::osd/nc::recorder）
    add_subdirectory(iot_agent)     # IoT 平台对接代理（依赖 nc::mqtt_client）
endif()
