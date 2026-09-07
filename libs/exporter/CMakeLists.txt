# ---------------------------------------------------------------------------
# nc::exporter —— 指标导出库（自 gw_av100/application/lib-exporter 复制，源码未改动）
# 原 target 名 libexporter → nc::exporter；
# 原 CMake 直接引用 lib-mq 的 build-arm/liblibmq.a 预编译产物，
# 此处改为标准 target 依赖 nc::mq（monorepo 内源码级构建）
# ---------------------------------------------------------------------------
add_library(nc_exporter STATIC
    src/metrics_collector.cpp
    src/base_exporter.cpp
)
add_library(nc::exporter ALIAS nc_exporter)

target_include_directories(nc_exporter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nc_exporter
    PUBLIC
        nc::mq
        pthread
    PRIVATE
        nc::cjson
        nc::compile_options
)
