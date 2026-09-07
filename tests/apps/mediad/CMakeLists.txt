# ---------------------------------------------------------------------------
# tests/apps/mediad/ —— mediad 进程级测试 (镜像 apps/mediad/src/ 模块)
#
# 每个子目录对应 mediad 的一个业务模块:
#   config/   → 配置模块 (ConfigStore 加载/合并/校验/OSD 规范化)
#
# 强约束: 不准在 apps/mediad/src/<module>/ 下面建 tests/ 目录
# ---------------------------------------------------------------------------
add_subdirectory(config)
