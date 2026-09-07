# ---------------------------------------------------------------------------
# 公共编译选项：所有 nc:: target 通过 nc::compile_options 继承统一设置
# ---------------------------------------------------------------------------
if(TARGET nc_compile_options)
    return()
endif()

add_library(nc_compile_options INTERFACE)
add_library(nc::compile_options ALIAS nc_compile_options)

target_compile_features(nc_compile_options INTERFACE cxx_std_17)

target_compile_options(nc_compile_options INTERFACE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-Og -g>
)
