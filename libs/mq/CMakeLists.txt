# ---------------------------------------------------------------------------
# nc::mq —— ZeroMQ 消息封装库（自 gw_av100/application/lib-mq 复制，源码未改动）
# 原 target 名 libmq → nc::mq；toolchain 由仓库集中管理，不再自带
# ---------------------------------------------------------------------------
add_library(nc_mq STATIC
    src/inproc_queue.cpp
    src/ipc_queue.cpp
    src/message_manager.cpp
    src/req_rep_queue.cpp
)
add_library(nc::mq ALIAS nc_mq)

target_include_directories(nc_mq PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nc_mq
    PUBLIC
        nc::cppzmq   # zmq.hpp 头 + libzmq（平台差异由 third_party/deps.cmake 解析）
        pthread
    PRIVATE
        nc::compile_options
)
