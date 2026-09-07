/**
 * @file token_store.h
 * @brief TB token 本地持久化
 *
 * 两个职责：
 *   1) 本地文件存 token（JSON 格式：{"token":"xxx","timestamp":...}）
 *      设备重启后直接从文件读，不用再走网络 provision
 *   2) 拿到 token 后通过 IPC 广播给其他进程（mediad / ota_agent 等）
 *      它们拿到 token 才能做自己的事（比如 mediad 用 token 推 SRS）
 *
 * 注意：这是 TB 特有的（TB 用 accessToken 当凭据）。
 * AWS 用证书文件，不需要这个。
 */
#pragma once

#include <string>

namespace iot_agent {

class TokenStore {
public:
    /**
     * 从文件加载 token
     * @param path token 文件路径
     * @param tokenOut 输出参数：读到的 token
     * @return 是否成功（文件不存在 / 解析失败 / token 为空都返回 false）
     */
    static bool load(const std::string& path, std::string& tokenOut);

    /**
     * 把 token 存到文件
     * @param path token 文件路径（目录不存在会自动创建）
     * @param token 要存的 token
     * @return 是否成功
     */
    static bool save(const std::string& path, const std::string& token);
};

} // namespace iot_agent
