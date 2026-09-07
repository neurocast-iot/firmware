# ---------------------------------------------------------------------------
# third_party：开源依赖统一管理 —— 本文件是全部第三方依赖的唯一声明入口
#
# 接入方式：
#   packages/  源码包（tar.gz + SHA256 密封）：构建期解包，源码树不落地
#   src/       可编辑源码（git submodule 或目录入库）：需深度修改的库
#
# 接入规则：
#   - CMake 体系的库走 nc_fetch_dep 三级查找（src/ → packages/ → URL）
#   - 版本信息编入包文件名，换版本必换包 → 必改哈希 → 必过 review
#   - 平台特定依赖（ARM 交叉编译用的 vendor SDK 等）不在此文件出现
# ---------------------------------------------------------------------------
include(FetchContent)

# nc_fetch_dep(<name> <filename> <url> <sha256>)
# 三级查找的依赖声明，优先级从高到低：
#   ① third_party/src/<name>/       可编辑源码（改动即时生效，git 跟踪）
#   ② third_party/packages/<file>   离线包（SHA256 校验）
#   ③ <url> 网络下载（SHA256 校验）
function(nc_fetch_dep name filename url sha256)
    set(_src "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/${name}")
    set(_local "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/packages/${filename}")
    if(EXISTS "${_src}/CMakeLists.txt")
        message(STATUS "nc_fetch_dep(${name}): using editable source ${_src}")
        FetchContent_Declare(${name} SOURCE_DIR "${_src}")
    elseif(EXISTS "${_local}")
        message(STATUS "nc_fetch_dep(${name}): using local package ${_local}")
        FetchContent_Declare(${name}
            URL "${_local}"
            URL_HASH SHA256=${sha256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
    else()
        message(STATUS "nc_fetch_dep(${name}): downloading ${url}")
        FetchContent_Declare(${name}
            URL "${_url}"
            URL_HASH SHA256=${sha256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
    endif()
endfunction()

# 全仓统一静态链接
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- libzmq 4.3.5（FetchContent 源码级拉取）---
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(WITH_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_CPACK OFF CACHE BOOL "" FORCE)
set(ENABLE_WS OFF CACHE BOOL "" FORCE)
set(WITH_LIBBSD OFF CACHE BOOL "" FORCE)

nc_fetch_dep(libzmq zeromq-4.3.5.tar.gz
    https://github.com/zeromq/libzmq/releases/download/v4.3.5/zeromq-4.3.5.tar.gz
    6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43
)
FetchContent_MakeAvailable(libzmq)

# --- cppzmq 4.11.0（header-only，聚合为 nc::cppzmq）---
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
nc_fetch_dep(cppzmq cppzmq-4.11.0.tar.gz
    https://github.com/zeromq/cppzmq/archive/refs/tags/v4.11.0.tar.gz
    280698994de3bbf9e06839b06634c5dd3062f094a9b52487800790652ce52390
)
FetchContent_MakeAvailable(cppzmq)
add_library(nc_cppzmq INTERFACE)
add_library(nc::cppzmq ALIAS nc_cppzmq)
target_link_libraries(nc_cppzmq INTERFACE cppzmq-static)

# --- cJSON 1.7.19（源码包，聚合为 nc::cjson）---
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "" FORCE)
nc_fetch_dep(cjson cJSON-1.7.19.tar.gz
    https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz
    7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
)
FetchContent_MakeAvailable(cjson)
add_library(nc_cjson INTERFACE)
add_library(nc::cjson ALIAS nc_cjson)
target_include_directories(nc_cjson INTERFACE ${cjson_SOURCE_DIR})
target_link_libraries(nc_cjson INTERFACE cjson)

# --- spdlog 1.17.0（源码包，聚合为 nc::spdlog）---
# 仅供 libs/common 日志门面内部使用（PRIVATE 链接），其它模块不直接依赖
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
nc_fetch_dep(spdlog spdlog-1.17.0.tar.gz
    https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
    2a7a065d173a4cb386645b005b701a9a0f71e9232b2230d08955d3bbc88c4c80
)
FetchContent_MakeAvailable(spdlog)
add_library(nc_spdlog INTERFACE)
add_library(nc::spdlog ALIAS nc_spdlog)
target_link_libraries(nc_spdlog INTERFACE spdlog::spdlog)

# --- OpenSSL（系统安装，消费方链 nc::openssl）---
# 板端私有版走 vendor openssl-1.1-fit（ARM 交叉编译静态库），
# 开源版直接用系统 OpenSSL，apt 装 libssl-dev 即可
find_package(OpenSSL REQUIRED)
add_library(nc_ssl INTERFACE)
target_link_libraries(nc_ssl INTERFACE OpenSSL::SSL)
add_library(nc_crypto INTERFACE)
target_link_libraries(nc_crypto INTERFACE OpenSSL::Crypto)
# 聚合目标: 消费方链 nc::openssl 即可
add_library(nc_openssl_iface INTERFACE)
add_library(nc::openssl ALIAS nc_openssl_iface)
target_link_libraries(nc_openssl_iface INTERFACE nc_ssl nc_crypto)

# --- libcurl（系统安装，消费方链 nc::curl）---
# 板端私有版走 vendor curl-7.88.1（ARM 交叉编译静态库），
# 开源版直接用系统 libcurl，apt 装 libcurl4-openssl-dev 即可
find_package(CURL REQUIRED)
add_library(nc_curl INTERFACE)
target_link_libraries(nc_curl INTERFACE CURL::libcurl)
# 聚合目标: curl 头文件 + 传递链接依赖
add_library(nc_curl_interface INTERFACE)
add_library(nc::curl ALIAS nc_curl_interface)
target_link_libraries(nc_curl_interface INTERFACE nc_curl pthread ${CMAKE_DL_LIBS})

# --- GoogleTest + libmicrohttpd 不在本文件声明 ---
# 测试专用依赖已在 tests/CMakeLists.txt 用 find_package + 系统包引入
# （libgtest-dev / libmicrohttpd-dev），apt 装包更轻
