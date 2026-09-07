# ---------------------------------------------------------------------------
# nc::http —— HTTP 工具包（上传 + 下载，基于 libcurl）
#
# 职责：
#   - HttpClient  统一 HTTP 客户端（上传/下载/断点续传/进度回调）
#                 返回 nc::http::DownloadResult（一次 HTTP 调用结果）
#   - download    高层下载策略 (重试 + 磁盘预检 + 取消)
#                 自由函数 nc::http::downloadWithPolicy(url, path, policy)
#                 返回 nc::http::DownloadWithPolicyResult（多次尝试 + 重试统计）
#   - upload      高层上传策略 (重试 + 取消)
#                 自由函数 nc::http::uploadWithPolicy(url, path, policy)
#                 返回 nc::http::UploadWithPolicyResult（多次尝试 + 重试统计）
#
# 依赖 nc::curl（libcurl 静态库）和 nc::common（日志），
# 消费方仅板端，x86 单测也用，平台门控看 libs/CMakeLists.txt。
# ---------------------------------------------------------------------------
add_library(nc_http STATIC
    src/http_client.cpp
    src/download.cpp
    src/upload.cpp
)
add_library(nc::http ALIAS nc_http)

target_include_directories(nc_http
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(nc_http
    PUBLIC  nc::common
    PRIVATE nc::curl nc::compile_options
)
