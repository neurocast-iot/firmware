/**
 * @file tunnel_manager.cpp
 * @brief 反向隧道管理器实现
 *
 * 并发设计说明：
 *   - 状态（m_pid/m_type/m_keyFile）只在 m_mutex 下读写
 *   - 监控线程全程不拿状态锁：它只读启动时传进来的 pid/端口副本，
 *     退出前只把 m_monitorRunning 置 false。状态清理由 stop/start/析构
 *     里 join 之后的逻辑统一做。这样 stop 在锁内 join 也不会死锁
 *   - 监控线程的 wait_for 用独立的 m_monitorMutex，stop 只需置 false + notify
 */
#include "tunnel/tunnel_manager.h"

#include "nc/common/log_utils.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

/* execve 需要完整环境数组，从全局 environ 拷出来再追加自定义变量 */
extern char** environ;

namespace iot_agent {

namespace {

/* 首轮存活检查延迟：参数错 / 二进制缺失的子进程会秒退，2 秒就能抓到 */
constexpr auto kFirstCheckDelay = std::chrono::seconds(2);

/* 稳定运行后的巡检周期：10 分钟数一次连接数，无人使用自动回收 */
constexpr auto kCheckInterval = std::chrono::minutes(10);

/** 端口必须是纯数字，防止把奇怪的东西拼进 shell/netstat 命令 */
bool isDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

} // namespace

/* ====================================================================
 * 构造 / 析构
 * ==================================================================== */

TunnelManager::TunnelManager() {
    /* 默认接真实现；单测里用 setXxxFn 换成桩 */
    m_spawnFn = [](const SpawnRequest& req) { return doSpawn(req); };
    m_killFn  = [](pid_t pid) { doKillAndReap(pid); };
    m_countConnFn = [](const std::string& port) { return doCountConnections(port); };
}

TunnelManager::~TunnelManager() {
    /* 进程退出前把隧道收干净：停监控线程 + 杀子进程 + 删私钥 */
    std::lock_guard<std::mutex> lk(m_mutex);
    stopCurrentLocked();
}

/* ====================================================================
 * startFrp：拉起 frpc 隧道
 * ==================================================================== */
std::string TunnelManager::startFrp(const FrpTunnelParams& p) {
    /* ---- 参数校验（锁外做，别占着锁） ---- */
    if (p.serverAddr.empty() || p.serverPort.empty() ||
        p.token.empty() || p.remotePort.empty()) {
        return "missing required params (serverAddr/serverPort/token/remotePort)";
    }
    if (!isDigits(p.serverPort) || !isDigits(p.remotePort)) {
        return "serverPort/remotePort must be numeric";
    }
    if (!p.proxyType.empty() && p.proxyType != "tcp") {
        /* frpc 命令行模式只做 tcp 转发 */
        return "unsupported proxy type: " + p.proxyType + " (only tcp)";
    }
    if (!p.authMethod.empty() && p.authMethod != "token") {
        /* 服务端目前只发 token 方式，其他值先记着，按 token 继续走 */
        NC_LOGW("[Tunnel] frp 鉴权方式是 {}，命令行模式只支持 token，按 token 处理",
                p.authMethod.c_str());
    }

    const std::string localPort = p.localPort.empty() ? "22" : p.localPort;
    if (!isDigits(localPort)) return "localPort must be numeric";
    const std::string proxyName = p.proxyName.empty() ? "ssh" : p.proxyName;

    std::lock_guard<std::mutex> lk(m_mutex);

    /* 覆盖式重启：旧的先停掉（没在跑则是空操作），保证同一时刻只有一条隧道 */
    stopCurrentLocked();

    SpawnRequest req;
    req.program = m_frpcPath;
    req.args = {
        "frpc", "tcp",
        "--server_addr", p.serverAddr + ":" + p.serverPort,
        "--token",       p.token,
        "--local_ip",    "127.0.0.1",   /* 云端传的 localIP 一律不认，见头文件说明 */
        "--local_port",  localPort,
        "--remote_port", p.remotePort,
        "--proxy_name",  proxyName,
    };

    const pid_t pid = m_spawnFn(req);
    if (pid <= 0) return "spawn frpc failed";

    m_pid = pid;
    m_type = TunnelType::Frp;
    m_checkPort = localPort;
    m_keyFile.clear();
    startMonitorLocked();

    NC_LOGI("[Tunnel] frpc 已启动 pid={} proxy={} 远端 {}:{} -> 本地 127.0.0.1:{}",
            static_cast<long>(pid), proxyName.c_str(),
            p.serverAddr.c_str(), p.remotePort.c_str(), localPort.c_str());
    return "";
}

/* ====================================================================
 * startSshTunnel：拉起 dbclient SSH 反向隧道
 * ==================================================================== */
std::string TunnelManager::startSshTunnel(const SshTunnelParams& p) {
    if (p.serverAddr.empty() || p.serverPort.empty() ||
        p.username.empty() || p.remotePort.empty()) {
        return "missing required params (serverAddr/serverPort/username/remotePort)";
    }
    if (!isDigits(p.serverPort) || !isDigits(p.remotePort)) {
        return "serverPort/remotePort must be numeric";
    }
    const bool useKey = !p.privateKey.empty();
    const bool usePwd = !p.password.empty();
    if (!useKey && !usePwd) return "missing auth: need privateKey or password";

    const std::string localPort = p.localPort.empty() ? "22" : p.localPort;
    if (!isDigits(localPort)) return "localPort must be numeric";

    std::lock_guard<std::mutex> lk(m_mutex);
    stopCurrentLocked();

    /* 私钥优先：落盘成 0600 的临时文件给 dbclient -i 用；
     * 私钥和密码都带时按私钥处理，密码忽略 */
    if (useKey) {
        std::string pem = p.privateKey;
        if (pem.back() != '\n') pem += '\n';   /* PEM 要求末尾有换行 */
        if (!writeKeyFile(kSshKeyFile, pem)) return "write ssh key file failed";
        m_keyFile = kSshKeyFile;
        if (usePwd) NC_LOGW("[Tunnel] privateKey 和 password 都带了，按 privateKey 处理");
    }

    SpawnRequest req;
    req.program = m_sshClientPath;
    /* -y：自动接受服务器 host key（设备上没法交互输 yes） */
    req.args = {"dbclient", "-y"};
    if (useKey) {
        req.args.push_back("-i");
        req.args.push_back(kSshKeyFile);
    }
    req.args.push_back("-p");
    req.args.push_back(p.serverPort);
    /* -R 远端端口:127.0.0.1:本地端口：把设备的端口反向映射到服务器上 */
    req.args.push_back("-R");
    req.args.push_back(p.remotePort + ":127.0.0.1:" + localPort);
    req.args.push_back(p.username + "@" + p.serverAddr);
    /* dbclient 没有命令行传密码的选项，走 DROPBEAR_PASSWORD 环境变量 */
    if (!useKey && usePwd) {
        req.envs.push_back("DROPBEAR_PASSWORD=" + p.password);
    }

    const pid_t pid = m_spawnFn(req);
    if (pid <= 0) {
        /* 没拉起来，刚落的私钥文件一并清掉 */
        deleteFileIfExist(m_keyFile);
        m_keyFile.clear();
        return "spawn dbclient failed";
    }

    m_pid = pid;
    m_type = TunnelType::Ssh;
    m_checkPort = localPort;
    startMonitorLocked();

    NC_LOGI("[Tunnel] SSH 反向隧道已启动 pid={} 远端 {}:{} -> 本地 127.0.0.1:{}",
            static_cast<long>(pid), p.serverAddr.c_str(),
            p.remotePort.c_str(), localPort.c_str());
    return "";
}

/* ====================================================================
 * stopFrp / stopSshTunnel：幂等停止
 * ==================================================================== */
std::string TunnelManager::stopFrp() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_type == TunnelType::None) return "";   /* 没在跑：重复 stop 当成功 */
    if (m_type != TunnelType::Frp) {
        /* 跑的是 SSH 隧道：frp 本来就没开，同样当成功（云端语义是"关掉 frp"） */
        NC_LOGI("[Tunnel] stop_frp 收到时跑的是 SSH 隧道，frp 本来就没开，忽略");
        return "";
    }
    stopCurrentLocked();
    NC_LOGI("[Tunnel] frpc 隧道已停止");
    return "";
}

std::string TunnelManager::stopSshTunnel() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_type == TunnelType::None) return "";
    if (m_type != TunnelType::Ssh) {
        NC_LOGI("[Tunnel] stop_ssh_tunnel 收到时跑的是 frp 隧道，SSH 本来就没开，忽略");
        return "";
    }
    stopCurrentLocked();
    NC_LOGI("[Tunnel] SSH 反向隧道已停止");
    return "";
}

bool TunnelManager::running() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_type != TunnelType::None;
}

/* ====================================================================
 * 停止 / 监控的核心逻辑（锁内调用）
 * ==================================================================== */

void TunnelManager::stopCurrentLocked() {
    if (m_type == TunnelType::None) return;

    /* 先让监控线程退出来。它只做检查不做清理，
     * 收到停止信号后很快退出，join 不会卡 10 分钟 */
    m_monitorRunning.store(false);
    m_monitorCv.notify_all();
    if (m_monitorThread.joinable()) m_monitorThread.join();

    /* 再杀进程并收尸（SIGTERM 优雅退出，不行就 SIGKILL） */
    if (m_pid > 0) {
        NC_LOGI("[Tunnel] 停止隧道进程 pid={}", static_cast<long>(m_pid));
        m_killFn(m_pid);
    }

    deleteFileIfExist(m_keyFile);
    m_pid = -1;
    m_type = TunnelType::None;
    m_keyFile.clear();
    m_checkPort.clear();
}

void TunnelManager::startMonitorLocked() {
    m_monitorRunning.store(true);
    /* 副本传进线程：监控线程不读成员状态，避免和 stop 抢锁 */
    const pid_t pid = m_pid;
    const std::string checkPort = m_checkPort;
    m_monitorThread = std::thread([this, pid, checkPort] {
        monitorLoop(pid, checkPort);
    });
}

void TunnelManager::monitorLoop(pid_t pid, std::string checkPort) {
    NC_LOGI("[Tunnel] 隧道监控已启动：{} 秒后先查一次进程存活，之后每 {} 分钟数一次连接数",
            static_cast<long>(kFirstCheckDelay.count()),
            static_cast<long>(kCheckInterval.count()));

    bool firstRound = true;
    while (true) {
        const bool isFirstRound = firstRound;

        /* wait_for 期间持有 monitorMutex 但会释放锁睡眠；
         * 主动 stop 会置 false + notify，让它立刻醒来退出 */
        {
            std::unique_lock<std::mutex> lk(m_monitorMutex);
            m_monitorCv.wait_for(lk, isFirstRound ? kFirstCheckDelay : kCheckInterval,
                                 [this] { return !m_monitorRunning.load(); });
        }
        if (!m_monitorRunning.load()) break;

        /* ---- 1) 进程还活着吗（顺便 waitpid 收尸，防止僵尸进程） ---- */
        int st = 0;
        const pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(st)) {
                NC_LOGW("[Tunnel] 隧道进程自己退出了 code={}（多半是参数错或连不上服务器）",
                        WEXITSTATUS(st));
            } else {
                NC_LOGW("[Tunnel] 隧道进程被信号干掉了 sig={}", WTERMSIG(st));
            }
            break;
        }
        if (r < 0) {
            /* ECHILD 等：进程已经不在了（被别处收过尸），同样按退出处理 */
            NC_LOGW("[Tunnel] waitpid 查不到隧道进程 pid={}，监控退出", static_cast<long>(pid));
            break;
        }
        firstRound = false;

        /* 首轮只查存活（刚启动不可能有人连），连接检查从下一轮开始 */
        if (isFirstRound) {
            NC_LOGI("[Tunnel] 启动 {} 秒后存活检查通过 pid={}",
                    static_cast<long>(kFirstCheckDelay.count()), static_cast<long>(pid));
            continue;
        }

        /* ---- 2) 数一数有多少人正通过隧道连进来，没人用就自动回收 ---- */
        const int conn = m_countConnFn(checkPort);
        if (conn < 0) {
            /* 查询失败（比如 popen 起不来）：宁可多跑一轮也不能误杀 */
            NC_LOGW("[Tunnel] 连接数查询失败，这轮跳过检查");
            continue;
        }
        if (conn == 0) {
            NC_LOGI("[Tunnel] 隧道已 {} 分钟无人使用，自动回收进程 pid={}",
                    static_cast<long>(kCheckInterval.count()), static_cast<long>(pid));
            m_killFn(pid);
            break;
        }
        NC_LOGI("[Tunnel] 连接数={} 隧道继续运行 pid={}", conn, static_cast<long>(pid));
    }

    /* 退出前置 false，让外层 stop 的 join 立刻能返回。
     * 状态清理（m_type/m_pid）留给 stop/start/析构统一做，这里不碰锁 */
    m_monitorRunning.store(false);
}

/* ====================================================================
 * 默认进程操作实现
 * ==================================================================== */

pid_t TunnelManager::doSpawn(const SpawnRequest& req) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        NC_LOGE("[Tunnel] fork 失败: {}", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* 子进程：组 argv / envp 后 exec，成功就不回来了 */

        /* argv：按 SpawnRequest 约定 args[0] 就是程序名，整个 args 直接作为
         * argv 传给 execve。execve 第一个参数才是要执行的文件（req.program），
         * argv[0] 的内容它并不在意。注意别再把 req.program 额外塞成 argv[0]，
         * 否则程序名会重复一次（frpc frpc tcp ... 会被当成未知子命令秒退 code=1） */
        std::vector<char*> argv;
        argv.reserve(req.args.size() + 1);
        for (const auto& a : req.args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        /* envp：当前环境 + 追加项。注意必须在 execve 前把内容都备齐 */
        std::vector<std::string> envStorage;
        envStorage.reserve(req.envs.size() + 32);
        for (char** e = environ; e && *e; ++e) envStorage.emplace_back(*e);
        for (const auto& kv : req.envs) envStorage.push_back(kv);

        std::vector<char*> envp;
        envp.reserve(envStorage.size() + 1);
        for (const auto& s : envStorage) envp.push_back(const_cast<char*>(s.c_str()));
        envp.push_back(nullptr);

        ::execve(req.program.c_str(), argv.data(), envp.data());

        /* 走到这说明 exec 失败（二进制不存在等），exit code 127 是惯例 */
        _exit(127);
    }
    return pid;
}

void TunnelManager::doKillAndReap(pid_t pid) {
    if (pid <= 0) return;

    if (::kill(pid, SIGTERM) == 0) {
        /* 给 500 毫秒优雅退出，不行再补 SIGKILL */
        for (int i = 0; i < 5; ++i) {
            ::usleep(100 * 1000);
            int st = 0;
            if (::waitpid(pid, &st, WNOHANG) == pid) return;   /* 已退出并收尸 */
        }
        ::kill(pid, SIGKILL);
    }
    /* kill 失败说明进程早不在了；这里阻塞等一次，把僵尸收掉 */
    int st = 0;
    ::waitpid(pid, &st, 0);
}

int TunnelManager::doCountConnections(const std::string& localPort) {
    /* busybox 没有 ss，用 netstat -tn 过滤。
     * 每个通过隧道的连接在本机会出现两条 ESTABLISHED 记录
     * （隧道进程侧 + sshd 侧），只数 "127.0.0.1:<port> " 开头的
     * 那条（服务端视角）避免重复计数。
     * localPort 已在入口处做过纯数字校验，这里拼进命令没有注入风险 */
    const std::string cmd =
        "netstat -tn 2>/dev/null | grep '127.0.0.1:" + localPort +
        " ' | grep -c ESTABLISHED";

    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) {
        NC_LOGW("[Tunnel] popen 失败，无法数连接数");
        return -1;
    }
    char buf[64] = {0};
    const char* got = ::fgets(buf, sizeof(buf), fp);
    ::pclose(fp);
    if (!got) return -1;
    return std::atoi(buf);
}

bool TunnelManager::writeKeyFile(const std::string& path, const std::string& content) {
    /* 直接以 0600 创建，避免"先创建后改权限"中间的裸奔窗口 */
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        NC_LOGE("[Tunnel] 私钥文件创建失败 path={} err={}", path.c_str(), strerror(errno));
        return false;
    }
    const ssize_t n = ::write(fd, content.data(), content.size());
    ::close(fd);
    if (n != static_cast<ssize_t>(content.size())) {
        NC_LOGE("[Tunnel] 私钥文件写入不完整 path={}", path.c_str());
        return false;
    }
    return true;
}

void TunnelManager::deleteFileIfExist(const std::string& path) {
    if (path.empty()) return;
    if (::unlink(path.c_str()) == 0) {
        NC_LOGI("[Tunnel] 已删除临时私钥文件 {}", path.c_str());
    }
    /* 文件本来就不存在就算了，不用报错 */
}

} // namespace iot_agent
