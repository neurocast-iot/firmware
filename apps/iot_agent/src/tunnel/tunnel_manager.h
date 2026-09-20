/**
 * @file tunnel_manager.h
 * @brief 反向隧道管理器：frpc / SSH 两种隧道统一管理
 *
 * 职责：
 *   接云端 RPC 指令（start_frp / stop_frp / start_ssh_tunnel / stop_ssh_tunnel），
 *   在设备上拉起或关掉对应的反向隧道子进程，并看护它的生命周期：
 *   - 同一时刻只允许一条隧道在跑（新 start 覆盖旧的，语义与 iot_live 一致）
 *   - 精确记录子进程 PID，stop 只杀自己拉起的进程
 *     （iot_live 用 pkill -f 'frpc' 按命令行关键字杀，有误伤风险，这里改掉）
 *   - 启动后 2 秒先查一次进程存活：参数错、二进制缺失的失败立刻能发现，
 *     不像 iot_live 用 system() 拉起后对失败毫无感知
 *   - 之后每 10 分钟数一次本地端口的连接数，没人用了自动回收（按需开启、用完即走）
 *   - 隧道进程自己挂了（连不上服务器、被信号杀）也能被发现，状态回归空闲
 *
 * 两种隧道的原理是一样的（都是反向 TCP 隧道）：
 *   设备主动连公网服务器，把设备的 127.0.0.1:<localPort>（SSH 就是 22）
 *   映射到服务器的 <remotePort>，运维从服务器侧 ssh -p <remotePort> 连进设备。
 *   - frp   ：frpc 私有协议，token 鉴权，内置断线重连，需要服务端跑 frps
 *   - ssh -R：dbclient 反向隧道，密码/私钥鉴权，天然加密，任意 sshd 都能用
 */
#pragma once

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iot_agent {

/** 隧道类型 */
enum class TunnelType {
    None,   /* 没有隧道在跑 */
    Frp,    /* frpc 反向隧道（连 frps 服务器） */
    Ssh,    /* dbclient SSH 反向隧道（ssh -R） */
};

/** start_frp 指令参数（RpcHandler 从 JSON 解析后填进来） */
struct FrpTunnelParams {
    std::string serverAddr;   /* frps 服务器地址 */
    std::string serverPort;   /* frps 服务器端口 */
    std::string authMethod;   /* frp 鉴权方式（如 token）。命令行模式只支持 token，
                               * 其他值仅记告警并按 token 处理 */
    std::string token;        /* frp 鉴权令牌 */
    std::string proxyType;    /* 代理类型，目前只支持 tcp */
    std::string localPort;    /* 设备本地被转发的端口（SSH 就是 22），缺省 22 */
    std::string remotePort;   /* 服务端分配的公网端口 */
    std::string proxyName;    /* 代理名（服务端格式是 <deviceUid>-ssh），缺省 ssh */
    /* 注意：故意没有 localIp 字段——云端 params 里传的 localIP 一律不认，
     * 强制用 127.0.0.1。隧道只允许转发设备自己的服务，
     * 防止设备被利用去转发访问内网里的其他机器（与 iot_live 的做法一致） */
};

/** start_ssh_tunnel 指令参数 */
struct SshTunnelParams {
    std::string serverAddr;   /* SSH 服务器地址 */
    std::string serverPort;   /* SSH 服务器端口 */
    std::string username;     /* SSH 登录用户名 */
    std::string privateKey;   /* PEM 私钥内容（与 password 二选一，优先用私钥） */
    std::string password;     /* SSH 密码（通过 DROPBEAR_PASSWORD 环境变量传给 dbclient） */
    std::string localPort;    /* 设备本地被转发的端口，缺省 22 */
    std::string remotePort;   /* 服务端分配的公网端口 */
    /* localIp 同样不认，强制 127.0.0.1，理由同上 */
};

class TunnelManager {
public:
    TunnelManager();
    ~TunnelManager();

    TunnelManager(const TunnelManager&) = delete;
    TunnelManager& operator=(const TunnelManager&) = delete;

    /* ====================================================================
     * 进程操作注入接口
     * 生产代码用默认实现（fork/execve、kill 收尸、netstat 数连接）；
     * 单元测试替换成桩，避免真的拉起进程、真的杀进程。
     * ==================================================================== */

    /** 拉起子进程的请求：可执行文件路径 + 参数列表 + 追加环境变量（KEY=VALUE） */
    struct SpawnRequest {
        std::string program;             /* 可执行文件完整路径 */
        std::vector<std::string> args;   /* 参数列表，args[0] 是程序名（不含路径） */
        std::vector<std::string> envs;   /* 追加到当前环境后面的变量，可为空 */
    };
    using SpawnFn     = std::function<pid_t(const SpawnRequest&)>;
    using KillFn      = std::function<void(pid_t)>;
    using CountConnFn = std::function<int(const std::string& localPort)>;

    void setSpawnFn(SpawnFn fn) { m_spawnFn = std::move(fn); }
    void setKillFn(KillFn fn) { m_killFn = std::move(fn); }
    void setCountConnFn(CountConnFn fn) { m_countConnFn = std::move(fn); }

    /* ====================================================================
     * 业务接口：返回空串 = 成功，否则是给云端看的失败原因
     * ==================================================================== */

    /** 拉起 frpc 隧道（覆盖式：已有隧道在跑会先停掉） */
    std::string startFrp(const FrpTunnelParams& params);

    /** 拉起 dbclient SSH 反向隧道（覆盖式） */
    std::string startSshTunnel(const SshTunnelParams& params);

    /** 停掉 frpc 隧道；没在跑 / 跑的不是 frp 时幂等成功 */
    std::string stopFrp();

    /** 停掉 SSH 隧道；幂等语义同 stopFrp */
    std::string stopSshTunnel();

    /** 当前是否有隧道在跑（排查用） */
    bool running() const;

private:
    /* 停掉当前隧道（须在锁内调）：停监控线程 → 杀进程收尸 → 清状态 → 删私钥 */
    void stopCurrentLocked();

    /* 起监控线程（须在锁内调） */
    void startMonitorLocked();

    /* 监控线程主体：首轮 2 秒查进程存活，之后每 10 分钟数连接数，没人用就回收。
     * 只读启动时传进来的副本，不碰成员状态，避免和 stop 抢锁死锁 */
    void monitorLoop(pid_t pid, std::string checkPort);

    /* ---- 默认进程操作实现 ---- */
    static pid_t doSpawn(const SpawnRequest& req);         /* fork + execve */
    static void  doKillAndReap(pid_t pid);                 /* SIGTERM → SIGKILL → waitpid 收尸 */
    static int   doCountConnections(const std::string& localPort);  /* netstat 数连接 */

    /* 0600 权限直接创建私钥文件，避免"先创建后改权限"的窗口期 */
    static bool writeKeyFile(const std::string& path, const std::string& content);
    static void deleteFileIfExist(const std::string& path);

    /* ---- 成员 ---- */
    SpawnFn     m_spawnFn;
    KillFn      m_killFn;
    CountConnFn m_countConnFn;

    std::string m_frpcPath       = "/usr/bin/frpc";       /* frpc 二进制路径 */
    std::string m_sshClientPath  = "/usr/bin/dbclient";   /* SSH 客户端路径（dropbear 自带） */
    static constexpr const char* kSshKeyFile = "/tmp/nc_tunnel_key";

    mutable std::mutex m_mutex;          /* 保护以下所有状态 */
    pid_t       m_pid = -1;              /* 当前隧道子进程 PID */
    TunnelType  m_type = TunnelType::None;
    std::string m_checkPort;             /* 连接数检查的本地端口 */
    std::string m_keyFile;               /* 已落盘的私钥文件路径（stop 时删） */

    /* 监控线程专用锁：wait_for 打断用。与状态锁分开，
     * 因为监控线程不能拿状态锁（拿锁的 stop 会 join 它 → 死锁） */
    std::mutex               m_monitorMutex;
    std::condition_variable  m_monitorCv;
    std::thread              m_monitorThread;
    std::atomic<bool>        m_monitorRunning{false};
};

} // namespace iot_agent
