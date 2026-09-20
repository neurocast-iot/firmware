/**
 * @file test_rpc_handler.cpp
 * @brief RpcHandler 单元测试：RPC 分发逻辑 + 参数校验 + 响应格式
 *
 * 通过注入 fake callback 捕获 publish 和 IPC 调用，
 * 验证每个 RPC 指令的分发逻辑和错误处理。
 * 不测 fork/reboot 部分（restart/reset），那是系统调用，不是业务逻辑。
 */
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <utility>

#include "router/rpc_handler.h"
#include "tunnel/tunnel_manager.h"
#include "cJSON.h"

using namespace iot_agent;

namespace {
/**
 * 组一个带桩的 TunnelManager：spawn 记录拉起请求并返回假 pid，
 * 不会真的 fork、不会真的杀进程。注意：返回的是裸指针，调用方负责 delete
 */
TunnelManager* makeStubTunnelManager(
        std::vector<TunnelManager::SpawnRequest>* spawned) {
    auto* tm = new TunnelManager();
    tm->setSpawnFn([spawned](const TunnelManager::SpawnRequest& req) {
        spawned->push_back(req);
        return 4242;
    });
    tm->setKillFn([](pid_t) {});
    tm->setCountConnFn([](const std::string&) { return 1; });
    return tm;
}
} // namespace

/* ---- 测试夹具 ---- */
class RpcHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        /* 注入 fake publish 回调：记录每次调用的 (topic, payload) */
        handler_.setPublishCallback(
            [this](const std::string& topic, const std::string& payload) -> bool {
                published_.push_back({topic, payload});
                return true;
            });

        /* 注入 fake IPC 命令回调：记录每次调用的 (type, payload) */
        handler_.setIpcCommandCallback(
            [this](uint32_t type, const std::string& payload) -> bool {
                ipcCommands_.push_back({type, payload});
                return true;
            });

        /* 不设置 uploadService 和 config（默认 nullptr） */
    }

    /* 解析最后一条 publish 的 payload 为 JSON，方便断言 */
    std::string lastResponsePayload() const {
        if (published_.empty()) return "";
        return published_.back().second;
    }

    std::string lastResponseTopic() const {
        if (published_.empty()) return "";
        return published_.back().first;
    }

    /* 从 JSON 字符串里读某个字段 */
    static std::string jsonString(const std::string& json, const char* key) {
        cJSON* root = cJSON_Parse(json.c_str());
        if (!root) return "";
        const cJSON* item = cJSON_GetObjectItem(root, key);
        std::string result;
        if (cJSON_IsString(item) && item->valuestring) {
            result = item->valuestring;
        }
        cJSON_Delete(root);
        return result;
    }

    static bool jsonBool(const std::string& json, const char* key) {
        cJSON* root = cJSON_Parse(json.c_str());
        if (!root) return false;
        const cJSON* item = cJSON_GetObjectItem(root, key);
        bool result = cJSON_IsTrue(item);
        cJSON_Delete(root);
        return result;
    }

    RpcHandler handler_;
    std::vector<std::pair<std::string, std::string>> published_;  /* (topic, payload) */
    std::vector<std::pair<uint32_t, std::string>> ipcCommands_;   /* (type, payload) */
};

/* ====================================================================
 * 指令分发：未知 method
 * ==================================================================== */

/** 未实现的指令 → 回复 success=false + error 包含 "unsupported method" */
TEST_F(RpcHandlerTest, UnknownMethodReturnsUnsupported) {
    RpcRequest req;
    req.method = "nonExistentMethod";
    req.paramsJson = "{}";
    req.requestId = "100";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    ASSERT_EQ(published_.size(), 1u);
    EXPECT_EQ(lastResponseTopic(), "v1/devices/me/rpc/response/100");
    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("unsupported method"),
              std::string::npos);
}

/* ====================================================================
 * startLiveStream 指令
 * ==================================================================== */

/** startLiveStream → IPC 发出命令 + 回复成功 */
TEST_F(RpcHandlerTest, StartLiveStreamSendsIpcAndResponds) {
    RpcRequest req;
    req.method = "startLiveStream";
    req.paramsJson = "{}";
    req.requestId = "201";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    /* IPC 命令已发出（12010 = MEDIA_STREAM_START） */
    ASSERT_EQ(ipcCommands_.size(), 1u);
    EXPECT_EQ(ipcCommands_[0].first, 12010u);

    /* 回复成功 */
    ASSERT_EQ(published_.size(), 1u);
    EXPECT_EQ(lastResponseTopic(), "v1/devices/me/rpc/response/201");
    EXPECT_TRUE(jsonBool(lastResponsePayload(), "success"));
}

/** startLiveStream 带 accessToken → IPC 载荷包含 token */
TEST_F(RpcHandlerTest, StartLiveStreamForwardsAccessToken) {
    RpcRequest req;
    req.method = "startLiveStream";
    req.paramsJson = R"({"accessToken":"abc123"})";
    req.requestId = "202";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    ASSERT_EQ(ipcCommands_.size(), 1u);
    EXPECT_EQ(ipcCommands_[0].first, 12010u);
    /* IPC 载荷里应该包含 accessToken */
    EXPECT_NE(ipcCommands_[0].second.find("abc123"), std::string::npos);
    EXPECT_NE(ipcCommands_[0].second.find("accessToken"), std::string::npos);

    /* 回复成功 */
    EXPECT_TRUE(jsonBool(lastResponsePayload(), "success"));
}

/** startLiveStream 不带 accessToken → IPC 载荷为空 JSON */
TEST_F(RpcHandlerTest, StartLiveStreamNoToken) {
    RpcRequest req;
    req.method = "startLiveStream";
    req.paramsJson = "{}";
    req.requestId = "203";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    ASSERT_EQ(ipcCommands_.size(), 1u);
    /* 没有 accessToken 时，IPC 载荷是 "{}" */
    EXPECT_EQ(ipcCommands_[0].second, "{}");
}

/* ====================================================================
 * stopLiveStream 指令
 * ==================================================================== */

/** stopLiveStream → IPC 发出停止命令 + 回复成功 */
TEST_F(RpcHandlerTest, StopLiveStreamSendsIpcAndResponds) {
    RpcRequest req;
    req.method = "stopLiveStream";
    req.paramsJson = "{}";
    req.requestId = "301";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    /* IPC 命令已发出（12011 = MEDIA_STREAM_STOP） */
    ASSERT_EQ(ipcCommands_.size(), 1u);
    EXPECT_EQ(ipcCommands_[0].first, 12011u);

    /* 回复成功 */
    EXPECT_TRUE(jsonBool(lastResponsePayload(), "success"));
}

/** stopLiveStream 带 accessToken → IPC 载荷包含 token */
TEST_F(RpcHandlerTest, StopLiveStreamForwardsAccessToken) {
    RpcRequest req;
    req.method = "stopLiveStream";
    req.paramsJson = R"({"accessToken":"xyz789"})";
    req.requestId = "302";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    ASSERT_EQ(ipcCommands_.size(), 1u);
    EXPECT_NE(ipcCommands_[0].second.find("xyz789"), std::string::npos);
}

/* ====================================================================
 * uploadFile 指令：参数校验
 * ==================================================================== */

/** uploadFile 但上传服务没设置 → 回复失败 */
TEST_F(RpcHandlerTest, UploadFileNoService) {
    RpcRequest req;
    req.method = "uploadFile";
    req.paramsJson = R"({"fileType":"image","filePath":"/tmp/test.jpg","fileId":"f1"})";
    req.requestId = "401";
    req.source = "thingsboard";

    /* 不设置 uploadService（默认 nullptr） */
    handler_.handleRpc(req);

    ASSERT_EQ(published_.size(), 1u);
    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("upload service not ready"),
              std::string::npos);
}

/** uploadFile 参数不是合法 JSON → 回复失败 */
TEST_F(RpcHandlerTest, UploadFileInvalidParamsJson) {
    /* 设置一个假的 uploadService 指针，让代码走到参数解析 */
    /* 注意：这里用 reinterpret_cast 造一个非空指针，但不会真的调它的方法
     * 因为 JSON 解析在 enqueue 之前就失败了 */
    handler_.setUploadService(reinterpret_cast<FileUploadService*>(0x1));

    RpcRequest req;
    req.method = "uploadFile";
    req.paramsJson = "not json";
    req.requestId = "402";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("invalid params json"),
              std::string::npos);
}

/** uploadFile 缺少必填参数 → 回复失败 */
TEST_F(RpcHandlerTest, UploadFileMissingParams) {
    handler_.setUploadService(reinterpret_cast<FileUploadService*>(0x1));

    RpcRequest req;
    req.method = "uploadFile";
    /* 只有 fileType，缺 filePath 和 fileId */
    req.paramsJson = R"({"fileType":"image"})";
    req.requestId = "403";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("missing"),
              std::string::npos);
}

/** uploadFile 不支持的文件类型 → 回复失败 */
TEST_F(RpcHandlerTest, UploadFileUnsupportedType) {
    handler_.setUploadService(reinterpret_cast<FileUploadService*>(0x1));

    RpcRequest req;
    req.method = "uploadFile";
    req.paramsJson = R"({"fileType":"audio","filePath":"/tmp/test.mp3","fileId":"f2"})";
    req.requestId = "404";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("unsupported fileType"),
              std::string::npos);
}

/** uploadFile 文件不存在 → 回复失败 */
TEST_F(RpcHandlerTest, UploadFileFileNotFound) {
    handler_.setUploadService(reinterpret_cast<FileUploadService*>(0x1));

    RpcRequest req;
    req.method = "uploadFile";
    req.paramsJson = R"({"fileType":"image","filePath":"/tmp/nonexistent_test_file_12345.jpg","fileId":"f3"})";
    req.requestId = "405";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("file not found"),
              std::string::npos);
}

/* ====================================================================
 * IPC 回调未设置时的错误处理
 * ==================================================================== */

/** IPC 命令回调没设置 → startLiveStream 回复失败 */
TEST_F(RpcHandlerTest, StartLiveStreamNoIpcCallback) {
    /* 重新创建一个 handler，不设置 IPC 回调 */
    RpcHandler handler2;
    handler2.setPublishCallback(
        [this](const std::string& topic, const std::string& payload) -> bool {
            published_.push_back({topic, payload});
            return true;
        });
    /* 故意不调 setIpcCommandCallback */

    RpcRequest req;
    req.method = "startLiveStream";
    req.paramsJson = "{}";
    req.requestId = "501";
    req.source = "thingsboard";

    handler2.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("ipc command channel not available"),
              std::string::npos);
}

/* ====================================================================
 * publish 回调未设置时的行为
 * ==================================================================== */

/** publish 回调没设置 → handleRpc 不崩溃，只是日志警告 */
TEST_F(RpcHandlerTest, NoPublishCallbackNoCrash) {
    RpcHandler handler2;
    /* 不设置 publish 回调 */
    handler2.setIpcCommandCallback(
        [](uint32_t, const std::string&) { return true; });

    RpcRequest req;
    req.method = "startLiveStream";
    req.paramsJson = "{}";
    req.requestId = "601";
    req.source = "thingsboard";

    /* 不应该崩溃 */
    EXPECT_NO_THROW(handler2.handleRpc(req));
}

/* ====================================================================
 * 响应主题格式验证
 * ==================================================================== */

/** 不同 requestId → 回复到对应的 response 主题 */
TEST_F(RpcHandlerTest, ResponseTopicMatchesRequestId) {
    auto sendAndCheck = [this](const std::string& requestId) {
        published_.clear();
        RpcRequest req;
        req.method = "startLiveStream";
        req.paramsJson = "{}";
        req.requestId = requestId;
        req.source = "thingsboard";
        handler_.handleRpc(req);
        return lastResponseTopic();
    };

    EXPECT_EQ(sendAndCheck("1"), "v1/devices/me/rpc/response/1");
    EXPECT_EQ(sendAndCheck("999"), "v1/devices/me/rpc/response/999");
    EXPECT_EQ(sendAndCheck("abc-def"), "v1/devices/me/rpc/response/abc-def");
}

/* ====================================================================
 * 隧道指令：start_frp / stop_frp / start_ssh_tunnel / stop_ssh_tunnel
 * 用真 TunnelManager + 桩注入（不真拉进程），验证参数解析和回复格式
 * ==================================================================== */

/** 隧道管理器没设置 → 回复失败 */
TEST_F(RpcHandlerTest, StartFrpNoTunnelManager) {
    RpcRequest req;
    req.method = "start_frp";
    req.paramsJson = "{}";
    req.requestId = "701";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    ASSERT_EQ(published_.size(), 1u);
    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("tunnel manager not ready"),
              std::string::npos);
}

/** 服务端真实报文（端口是数字）→ 解析正确、localIP 被强制 127.0.0.1、回复成功 */
TEST_F(RpcHandlerTest, StartFrpParsesServerPayload) {
    std::vector<TunnelManager::SpawnRequest> spawned;
    TunnelManager* tm = makeStubTunnelManager(&spawned);
    handler_.setTunnelManager(tm);

    RpcRequest req;
    req.method = "start_frp";
    req.paramsJson = R"({
        "method": "token",
        "token": "abc123",
        "serverAddr": "47.121.24.61",
        "serverPort": 10000,
        "proxies": [{
            "name": "DEV123-ssh", "type": "tcp",
            "localIP": "192.168.1.5", "localPort": 22, "remotePort": 10002
        }]
    })";
    req.requestId = "702";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    /* 回复成功（iot_live 版不回复，这里必须有） */
    ASSERT_EQ(published_.size(), 1u);
    EXPECT_EQ(lastResponseTopic(), "v1/devices/me/rpc/response/702");
    EXPECT_TRUE(jsonBool(lastResponsePayload(), "success"));

    /* frpc 命令行内容：数字端口兼容、localIP 强制 127.0.0.1 */
    ASSERT_EQ(spawned.size(), 1u);
    std::string joined;
    for (const auto& a : spawned[0].args) joined += a + " ";
    EXPECT_NE(joined.find("--server_addr 47.121.24.61:10000 "), std::string::npos);
    EXPECT_NE(joined.find("--token abc123 "), std::string::npos);
    EXPECT_NE(joined.find("--local_ip 127.0.0.1 "), std::string::npos);
    EXPECT_NE(joined.find("--local_port 22 "), std::string::npos);
    EXPECT_NE(joined.find("--remote_port 10002 "), std::string::npos);
    EXPECT_NE(joined.find("--proxy_name DEV123-ssh "), std::string::npos);

    delete tm;
}

/** start_frp 参数不是合法 JSON → 回复失败且不拉进程 */
TEST_F(RpcHandlerTest, StartFrpInvalidParamsJson) {
    std::vector<TunnelManager::SpawnRequest> spawned;
    TunnelManager* tm = makeStubTunnelManager(&spawned);
    handler_.setTunnelManager(tm);

    RpcRequest req;
    req.method = "start_frp";
    req.paramsJson = "not json";
    req.requestId = "703";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("invalid params json"),
              std::string::npos);
    EXPECT_TRUE(spawned.empty());
    delete tm;
}

/** start_frp 缺必填参数（无 token）→ 回复失败且不拉进程 */
TEST_F(RpcHandlerTest, StartFrpMissingRequiredParams) {
    std::vector<TunnelManager::SpawnRequest> spawned;
    TunnelManager* tm = makeStubTunnelManager(&spawned);
    handler_.setTunnelManager(tm);

    RpcRequest req;
    req.method = "start_frp";
    req.paramsJson = R"({"serverAddr":"1.2.3.4","serverPort":10000,"proxies":[]})";
    req.requestId = "704";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("missing required params"),
              std::string::npos);
    EXPECT_TRUE(spawned.empty());
    delete tm;
}

/** 没开隧道时收到 stop_frp → 幂等，回复成功 */
TEST_F(RpcHandlerTest, StopFrpIdleReturnsSuccess) {
    TunnelManager* tm = makeStubTunnelManager(nullptr);
    handler_.setTunnelManager(tm);

    RpcRequest req;
    req.method = "stop_frp";
    req.paramsJson = "{}";
    req.requestId = "705";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_TRUE(jsonBool(lastResponsePayload(), "success"));
    delete tm;
}

/** stop_frp 隧道管理器没设置 → 回复失败 */
TEST_F(RpcHandlerTest, StopFrpNoTunnelManager) {
    RpcRequest req;
    req.method = "stop_frp";
    req.paramsJson = "{}";
    req.requestId = "706";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("tunnel manager not ready"),
              std::string::npos);
}

/** start_ssh_tunnel 带 privateKey → dbclient 命令正确、回复成功 */
TEST_F(RpcHandlerTest, StartSshTunnelParsesPayload) {
    std::vector<TunnelManager::SpawnRequest> spawned;
    TunnelManager* tm = makeStubTunnelManager(&spawned);
    handler_.setTunnelManager(tm);

    RpcRequest req;
    req.method = "start_ssh_tunnel";
    req.paramsJson = R"({
        "serverAddr": "1.2.3.4",
        "serverPort": 22,
        "username": "root",
        "privateKey": "-----BEGIN KEY-----\\nabc",
        "localIp": "10.0.0.9",
        "localPort": 22,
        "remotePort": 10003
    })";
    req.requestId = "707";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    ASSERT_EQ(published_.size(), 1u);
    EXPECT_TRUE(jsonBool(lastResponsePayload(), "success"));

    ASSERT_EQ(spawned.size(), 1u);
    EXPECT_EQ(spawned[0].program, "/usr/bin/dbclient");
    std::string joined;
    for (const auto& a : spawned[0].args) joined += a + " ";
    EXPECT_NE(joined.find("-R 10003:127.0.0.1:22 "), std::string::npos);
    EXPECT_NE(joined.find("root@1.2.3.4"), std::string::npos);

    delete tm;
}

/** stop_ssh_tunnel 隧道管理器没设置 → 回复失败 */
TEST_F(RpcHandlerTest, StopSshTunnelNoTunnelManager) {
    RpcRequest req;
    req.method = "stop_ssh_tunnel";
    req.paramsJson = "{}";
    req.requestId = "708";
    req.source = "thingsboard";

    handler_.handleRpc(req);

    EXPECT_FALSE(jsonBool(lastResponsePayload(), "success"));
    EXPECT_NE(jsonString(lastResponsePayload(), "error").find("tunnel manager not ready"),
              std::string::npos);
}
