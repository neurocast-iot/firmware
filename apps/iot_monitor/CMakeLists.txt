# ---------------------------------------------------------------------------
# iot_monitor —— 进程/磁盘监控应用
# TODO(第2步): 迁入现有 iot_monitor 源码（采集基于 /proc、statvfs 等通用接口）；
#   Anyka 特定探针（如有）改走 pal::ISystemInfo
# ---------------------------------------------------------------------------
add_executable(iot_monitor
    src/main.cpp   # TODO(第2步): 补充迁入的真实源文件
)

target_link_libraries(iot_monitor
    PRIVATE
        nc::common
        nc::mq
        nc::exporter
        nc::pal
        nc::pal_backend
        nc::compile_options
)
