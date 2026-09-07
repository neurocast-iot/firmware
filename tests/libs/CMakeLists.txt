# ---------------------------------------------------------------------------
# tests/libs/ —— 库单元测试 (镜像 libs/ 树)
#
# 每个子目录对应 libs/ 下某个库的单测:
#   http/         → nc::http (HttpClient 底层 + downloadWithPolicy 高层策略)
#   osd/          → osd:: (OSD 工具函数: 颜色匹配/字体大小/宽度计算)
#   config_sync/  → nc::config_sync (消息类型序列化/反序列化)
#   mq/           → nc::mq                 (TODO 未来)
#   common/       → nc::common             (TODO 未来)
#
# 强约束: 不准在 libs/<lib_name>/ 下面建 tests/ 目录, 必须把测试放这里。
# ---------------------------------------------------------------------------
add_subdirectory(http)
add_subdirectory(osd)
add_subdirectory(config_sync)
add_subdirectory(common)
