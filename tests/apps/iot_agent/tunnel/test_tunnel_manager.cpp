/**
 * @file test_tunnel_manager.cpp
 * @brief TunnelManager 单元测试：参数校验 + 命令构造 + 生命周期
 *
 * spawn/kill/countConn 全部注入桩，不会真的拉起进程、杀进程。
 * 监控线程的"2 秒首轮存活检查"用不存在的假 pid 验证（waitpid 返回 ECHILD，
 * 监控会按"进程已退出"处理）；10 分钟周期的连接数回收逻辑太慢，不在此测。
 */
#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "tunnel/tunnel_manager.h"

using namespace iot_agent;

namespace {
/* 私钥临时文件路径（与 TunnelManager 里的常量一致） */
constexpr const char* kKeyFile = "/tmp/nc_tunnel_key";
} // namespace

class TunnelManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::unlink(kKeyFile);   /* 上个测试可能残留私钥文件 */
        nextPid_ = 4242;

        tm_.setSpawnFn([this](const TunnelManager::SpawnRequest& req) {
            spawnRequests_.push_back(req);
            return nextPid_++;
        });
        tm_.setKillFn([this](pid_t pid) { killedPids_.push_back(pid); });
        tm_.setCountConnFn([](const std::string&) { return 1; });
    }

    void TearDown() override { ::unlink(kKeyFile); }

    /* 组一份合法的 start_frp 参数 */
    static FrpTunnelParams validFrp() {
        FrpTunnelParams p;
        p.serverAddr = "47.121.24.61";
        p.serverPort = "10000";
        p.authMethod = "token";
        p.token      = "abc123";
        p.proxyType  = "tcp";
        p.localPort  = "22";
        p.remotePort = "10002";
        p.proxyName  = "DEV123-ssh";
        return p;
    }

    /* 组一份合法的 start_ssh_tunnel 参数 */
    static SshTunnelParams validSsh() {
        SshTunnelParams p;
        p.serverAddr = "47.121.24.61";
        p.serverPort = "22";
        p.username   = "root";
        p.localPort  = "22";
        p.remotePort = "10003";
        return p;
    }

    /* 把 args 拼成一个字符串方便断言 */
    static std::string joinedArgs(const TunnelManager::SpawnRequest& req) {
        std::string joined;
        for (const auto& a : req.args) joined += a + " ";
        return joined;
    }

    std::vector<TunnelManager::SpawnRequest> spawnRequests_;
    std::vector<pid_t> killedPids_;
    pid_t nextPid_ = 4242;
    /* tm_ 放最后声明 → 最先析构：否则 tm_ 析构杀隧道时调 kill 桩，
     * 而桩里的容器已经先析构了 → use-after-free（double free 崩溃） */
    TunnelManager tm_;
};

/* ====================================================================
 * startFrp：参数校验
 * ==================================================================== */

/** 缺必填参数 → 报错，不会去拉进程 */
TEST_F(TunnelManagerTest, StartFrpMissingParams) {
    FrpTunnelParams p = validFrp();
    p.token = "";
    EXPECT_NE(tm_.startFrp(p), "");
    EXPECT_TRUE(spawnRequests_.empty());
    EXPECT_FALSE(tm_.running());
}

/** 端口不是纯数字 → 报错（防注入，端口要拼进 netstat 过滤命令） */
TEST_F(TunnelManagerTest, StartFrpRejectsNonNumericPort) {
    FrpTunnelParams p = validFrp();
    p.serverPort = "abc";
    EXPECT_NE(tm_.startFrp(p), "");
    EXPECT_TRUE(spawnRequests_.empty());
}

/** 只支持 tcp 代理，其他类型拒绝 */
TEST_F(TunnelManagerTest, StartFrpRejectsNonTcpProxy) {
    FrpTunnelParams p = validFrp();
    p.proxyType = "udp";
    const std::string err = tm_.startFrp(p);
    EXPECT_NE(err.find("proxy type"), std::string::npos);
    EXPECT_TRUE(spawnRequests_.empty());
}

/* ====================================================================
 * startFrp：命令构造
 * ==================================================================== */

/** 正常启动 → frpc 命令行参数正确；缺省值生效；localIP 强制 127.0.0.1 */
TEST_F(TunnelManagerTest, StartFrpBuildsCommandLine) {
    FrpTunnelParams p = validFrp();
    p.localPort = "";    /* 缺省 22 */
    p.proxyName = "";    /* 缺省 ssh */
    EXPECT_EQ(tm_.startFrp(p), "");

    ASSERT_EQ(spawnRequests_.size(), 1u);
    const auto& req = spawnRequests_[0];
    EXPECT_EQ(req.program, "/usr/bin/frpc");

    const std::string joined = joinedArgs(req);
    EXPECT_NE(joined.find("--server_addr 47.121.24.61:10000 "), std::string::npos);
    EXPECT_NE(joined.find("--token abc123 "), std::string::npos);
    /* 云端就算传了别的 localIP，这里也必须强制 127.0.0.1（防内网横向转发） */
    EXPECT_NE(joined.find("--local_ip 127.0.0.1 "), std::string::npos);
    EXPECT_NE(joined.find("--local_port 22 "), std::string::npos);
    EXPECT_NE(joined.find("--remote_port 10002 "), std::string::npos);
    EXPECT_NE(joined.find("--proxy_name ssh "), std::string::npos);
    EXPECT_TRUE(req.envs.empty());
    EXPECT_TRUE(tm_.running());
}

/* ====================================================================
 * 覆盖式重启与停止
 * ==================================================================== */

/** 已有隧道在跑时再 start → 旧进程被精确杀掉（按 PID，不是 pkill），再起新的 */
TEST_F(TunnelManagerTest, StartFrpOverwriteRestarts) {
    EXPECT_EQ(tm_.startFrp(validFrp()), "");
    EXPECT_EQ(tm_.startFrp(validFrp()), "");

    EXPECT_EQ(spawnRequests_.size(), 2u);
    ASSERT_EQ(killedPids_.size(), 1u);
    EXPECT_EQ(killedPids_[0], 4242);   /* 杀的是第一个 pid */
    EXPECT_TRUE(tm_.running());
}

/** 没在跑时 stop → 幂等成功，不碰任何进程 */
TEST_F(TunnelManagerTest, StopFrpIdempotentWhenIdle) {
    EXPECT_EQ(tm_.stopFrp(), "");
    EXPECT_TRUE(killedPids_.empty());
}

/** start 后 stop → 自己拉起的进程被杀，状态回归空闲 */
TEST_F(TunnelManagerTest, StopFrpStopsProcess) {
    EXPECT_EQ(tm_.startFrp(validFrp()), "");
    EXPECT_EQ(tm_.stopFrp(), "");

    ASSERT_EQ(killedPids_.size(), 1u);
    EXPECT_EQ(killedPids_[0], 4242);
    EXPECT_FALSE(tm_.running());

    /* 再 stop 一次：幂等，不会重复杀 */
    EXPECT_EQ(tm_.stopFrp(), "");
    EXPECT_EQ(killedPids_.size(), 1u);
}

/** 跑的是 SSH 隧道时收到 stop_frp → 类型不符，忽略（frp 本来就没开） */
TEST_F(TunnelManagerTest, StopFrpTypeMismatchIgnored) {
    SshTunnelParams p = validSsh();
    p.privateKey = "-----BEGIN KEY-----\nabc";   /* 没有鉴权信息会被拒，先补上 */
    EXPECT_EQ(tm_.startSshTunnel(p), "");
    EXPECT_EQ(tm_.stopFrp(), "");
    EXPECT_TRUE(killedPids_.empty());
    EXPECT_TRUE(tm_.running());   /* SSH 隧道还在 */

    EXPECT_EQ(tm_.stopSshTunnel(), "");
    ASSERT_EQ(killedPids_.size(), 1u);
    EXPECT_FALSE(tm_.running());
}

/* ====================================================================
 * startSshTunnel：私钥落盘与命令构造
 * ==================================================================== */

/** 私钥落盘成 0600 文件，stop 时删掉 */
TEST_F(TunnelManagerTest, StartSshTunnelWritesKeyFile) {
    SshTunnelParams p = validSsh();
    p.privateKey = "-----BEGIN RSA PRIVATE KEY-----\nMIIB";   /* 故意不带末尾换行 */
    EXPECT_EQ(tm_.startSshTunnel(p), "");

    struct stat st;
    ASSERT_EQ(::stat(kKeyFile, &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600);   /* 权限必须是 0600，不能裸奔 */

    /* 末尾应自动补了换行（PEM 格式要求） */
    FILE* fp = ::fopen(kKeyFile, "rb");
    ASSERT_NE(fp, nullptr);
    std::string content;
    char buf[256];
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), fp)) > 0) content.append(buf, n);
    ::fclose(fp);
    EXPECT_NE(content.find("-----BEGIN RSA PRIVATE KEY-----\nMIIB\n"), std::string::npos);

    /* stop 后私钥文件应被删掉，不留敏感信息 */
    EXPECT_EQ(tm_.stopSshTunnel(), "");
    EXPECT_EQ(::stat(kKeyFile, &st), -1);
}

/** 密码方式 → 走 DROPBEAR_PASSWORD 环境变量传，不落盘、不带 -i */
TEST_F(TunnelManagerTest, StartSshTunnelPasswordGoesToEnv) {
    SshTunnelParams p = validSsh();
    p.password = "secret123";
    EXPECT_EQ(tm_.startSshTunnel(p), "");

    ASSERT_EQ(spawnRequests_.size(), 1u);
    const auto& req = spawnRequests_[0];
    ASSERT_EQ(req.envs.size(), 1u);
    EXPECT_EQ(req.envs[0], "DROPBEAR_PASSWORD=secret123");
    EXPECT_EQ(joinedArgs(req).find("-i "), std::string::npos);   /* 没有 -i 参数 */

    struct stat st;
    EXPECT_EQ(::stat(kKeyFile, &st), -1);   /* 密码方式不写私钥文件 */
}

/** 私钥和密码都带 → 私钥优先，密码忽略 */
TEST_F(TunnelManagerTest, StartSshTunnelPrefersKeyOverPassword) {
    SshTunnelParams p = validSsh();
    p.privateKey = "-----BEGIN KEY-----\nabc";
    p.password   = "secret123";
    EXPECT_EQ(tm_.startSshTunnel(p), "");

    ASSERT_EQ(spawnRequests_.size(), 1u);
    EXPECT_TRUE(spawnRequests_[0].envs.empty());   /* 密码没进环境变量 */
    EXPECT_NE(joinedArgs(spawnRequests_[0]).find("-i /tmp/nc_tunnel_key "),
              std::string::npos);
}

/** 私钥密码都没有 → 报错，不拉进程 */
TEST_F(TunnelManagerTest, StartSshTunnelMissingAuth) {
    const std::string err = tm_.startSshTunnel(validSsh());
    EXPECT_NE(err.find("privateKey or password"), std::string::npos);
    EXPECT_TRUE(spawnRequests_.empty());
}

/** dbclient 命令行参数正确；localIp 强制 127.0.0.1 */
TEST_F(TunnelManagerTest, StartSshTunnelBuildsCommandLine) {
    SshTunnelParams p = validSsh();
    p.localPort = "";    /* 缺省 22 */
    p.privateKey = "-----BEGIN KEY-----\nabc";
    EXPECT_EQ(tm_.startSshTunnel(p), "");

    ASSERT_EQ(spawnRequests_.size(), 1u);
    const auto& req = spawnRequests_[0];
    EXPECT_EQ(req.program, "/usr/bin/dbclient");

    const std::string joined = joinedArgs(req);
    EXPECT_NE(joined.find("-y "), std::string::npos);                    /* 自动接受 host key */
    EXPECT_NE(joined.find("-p 22 "), std::string::npos);
    EXPECT_NE(joined.find("-R 10003:127.0.0.1:22 "), std::string::npos); /* 反向映射 */
    EXPECT_NE(joined.find("root@47.121.24.61"), std::string::npos);
}

/* ====================================================================
 * 监控线程：启动即失败场景
 * ==================================================================== */

/** 假 pid 不是我们的子进程 → 2 秒后首轮存活检查发现"进程退了"，
 *  监控线程退出且不崩；之后 stop 正常收尾 */
TEST_F(TunnelManagerTest, MonitorHandlesEarlyProcessExit) {
    EXPECT_EQ(tm_.startFrp(validFrp()), "");

    /* 等 3 秒：监控首轮 2 秒检查会发现 pid=4242 根本不存在 */
    std::this_thread::sleep_for(std::chrono::seconds(3));

    /* stop 收尾：join 不卡，kill 恰好一次，状态清干净 */
    EXPECT_EQ(tm_.stopFrp(), "");
    ASSERT_EQ(killedPids_.size(), 1u);
    EXPECT_EQ(killedPids_[0], 4242);
    EXPECT_FALSE(tm_.running());
}
