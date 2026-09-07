/**
 * @file ice_config.h
 * @brief ICE 服务器配置与域名解析工具（nc::rtc 公共类型）
 *
 * 设计（对齐 WebRTC 标准全候选模型）：
 *   一次协商携带 Host + Srflx + Relay 全部候选，ICE 连通性检查按优先级
 *   自动选路（局域网直连 > STUN 打洞 > TURN 中继），不做应用层串行降级。
 *   metaRTC 8.0 采集线程为递进叠加式（YangIceAgent.c）：
 *   iceCandidateType=Turn 时依次采集 Host + Srflx + Relay 三类候选。
 *
 * 候选策略由配置字段推导（见 candidateType()）：
 *   host 为空          → Host（仅局域网直连）
 *   host 有、无凭证    → Stun（Host + Srflx）
 *   host 有、有凭证    → Turn（Host + Srflx + Relay，全候选）
 */
#pragma once

#include <string>

namespace rtc {

/**
 * ICE 服务器配置（STUN/TURN 同一 coturn 实例，共用端口与长期凭证）
 */
struct IceConfig {
    std::string host;        ///< 域名或 IP，空 = 仅 Host 候选（局域网直连）
    int         port = 3478;
    std::string username;    ///< TURN 长期凭证（空 = 仅 STUN，无 Relay 候选）
    std::string password;

    /** 推导 metaRTC iceCandidateType：0=Host 1=Stun 2=Turn（枚举值与 YangIceCandidateType 对齐） */
    int candidateType() const {
        if (host.empty()) return 0;
        return username.empty() ? 1 : 2;
    }
};

/**
 * 域名解析（IPv4）：metaRTC 的 yang_addr_set 仅支持点分十进制 IP，
 * 不做 DNS 解析，STUN/TURN 域名必须在应用层先解析为 IP 再传入。
 *
 * 必须在每次建会话时调用（而非进程启动时缓存）：4G 场景开机时
 * 网络未必就绪，且长时间运行后服务器 IP 可能变更。
 *
 * @param host  域名或 IP 字符串
 * @param outIp 输出解析结果（点分十进制）
 * @return true=解析成功
 */
bool resolveHostIPv4(const std::string& host, std::string& outIp);

} // namespace rtc
