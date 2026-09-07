# ---------------------------------------------------------------------------
# nc::common —— 公共基础库
# 日志门面 NC_LOGx 直接转发给 spdlog 原生 API（PUBLIC 链接，头文件暴露 spdlog），
# 调用方用 {} 格式化，类型安全，无缓冲区限制
# ---------------------------------------------------------------------------
add_library(nc_common STATIC
    src/log_utils.cpp
    src/file_utils.cpp
    src/string_utils.cpp
    src/crypto_utils.cpp
    src/json_utils.cpp
)
add_library(nc::common ALIAS nc_common)

target_include_directories(nc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nc_common
    PUBLIC nc::spdlog
    PRIVATE nc::compile_options pthread nc::openssl nc::cjson
)
