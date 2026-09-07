# ---------------------------------------------------------------------------
# tests/apps/iot_agent/ —— iot_agent 进程级测试 (镜像 apps/iot_agent/src/ 模块)
#
# 每个子目录对应 iot_agent 的一个业务模块:
#   ota/      → OTA 模块 (manifest 解析/类型工具/ThingsBoard 通知)
#   config/   → 配置模块 (cloud_config_translator/agent_config)
#   router/   → 路由模块 (config_router 配置合并/变更检测)
#   cloud/    → 云平台模块 (token_store token 持久化)
#   upload/   → 上传模块 (file_upload_client)       (TODO 未来)
#
# 强约束: 不准在 apps/iot_agent/src/<module>/ 下面建 tests/ 目录
# ---------------------------------------------------------------------------
add_subdirectory(ota)
add_subdirectory(config)
add_subdirectory(router)
add_subdirectory(cloud)
