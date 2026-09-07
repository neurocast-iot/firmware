# ---------------------------------------------------------------------------
# nc_test_fake_http_server —— 测试用 fake HTTP server
#
# libmicrohttpd 1.0.0 包装, 业务方通过 builder.route("/path", handler)
# 注册路由, builder.start() 启在 127.0.0.1:0 (系统自动分配端口),
# server.url("/path") 拼完整 URL 给 HttpClient 访问。
#
# 为什么用 STATIC 库而不是把 .cpp 直接塞到每个 test_xxx:
#   - 多个 test exe 链接同一份产物, 改一次重编一次, 不重复
#   - 链接关系显式, 看 CMakeLists 知道这个测试需要 fake server
#
# 用法:
#   target_link_libraries(test_xxx PRIVATE nc_test_fake_http_server::nc_test_fake_http_server)
#   #include "fake_http_server.h"
# ---------------------------------------------------------------------------
add_library(nc_test_fake_http_server STATIC
    fake_http_server.cpp
)

add_library(nc_test_fake_http_server::nc_test_fake_http_server ALIAS nc_test_fake_http_server)

# PUBLIC: 消费方 #include "fake_http_server.h" 也要能找到
target_include_directories(nc_test_fake_http_server
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# libmicrohttpd 头路径: 自己的 .cpp 要用, 但消费方不用 (消费方只 include 我们自己的头)
target_include_directories(nc_test_fake_http_server
    PRIVATE ${MICROHTTPD_INCLUDE_DIRS}
)

# libmicrohttpd 库: 消费方通过 HttpClient 间接会用到吗? 不会, 但 PUBLIC 保证链接顺序正确
target_link_libraries(nc_test_fake_http_server
    PUBLIC ${MICROHTTPD_LIBRARIES}
)
