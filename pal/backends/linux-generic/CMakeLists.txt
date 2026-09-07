# ---------------------------------------------------------------------------
# pal backend：linux-generic（宿主机 x86）
# 文件模拟烧写、软件渲染 OSD、/proc 系统探针
# ---------------------------------------------------------------------------
add_library(nc_pal_backend STATIC
    osd_backend_soft.cpp
    fw_flash_file.cpp
    system_info_linux.cpp
)
add_library(nc::pal_backend ALIAS nc_pal_backend)

target_link_libraries(nc_pal_backend
    PUBLIC nc::pal
    PRIVATE nc::compile_options
)
