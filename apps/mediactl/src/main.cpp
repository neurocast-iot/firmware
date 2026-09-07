/**
 * @file main.cpp
 * @brief mediactl —— mediad 调试 CLI 客户端（curl 之于 http 服务）
 *
 * 直连 mediad 的 IPC 命令通道发命令、收应答，用于板上排障与链路联调：
 * 不经 MQTT/iot_live，单独验证 mediad 本身是否正常。
 *
 * 用法：
 *   mediactl [-e <endpoint>] snapshot [quality]
 *   mediactl [-e <endpoint>] status
 *   mediactl [-e <endpoint>] config get
 *   mediactl [-e <endpoint>] config set '<json-patch>'
 *   mediactl [-E <endpoint>] listen        # 订阅事件通道，Ctrl+C 退出
 *
 * 协议单一来源：消息编号直接引用 mediad 的 ipc/MediadMessages.h。
 */
#include "ipc/MediadMessages.h"

#include "libmq/req_rep_queue.h"
#include "libmq/message_manager.h"
#include "libmq/message_types.h"

#include "cJSON.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

const char* kDefaultCmdEndpoint   = "ipc:///tmp/nc_mediad_cmd.ipc";
const char* kDefaultEventEndpoint = "ipc:///tmp/nc_mediad_evt.ipc";
constexpr int kRequestTimeoutMs = 5000;

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

void usage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  mediactl [-e <cmd-endpoint>] snapshot [quality]\n"
        "  mediactl [-e <cmd-endpoint>] status\n"
        "  mediactl [-e <cmd-endpoint>] config get\n"
        "  mediactl [-e <cmd-endpoint>] config set '<json-patch>'\n"
        "  mediactl [-E <event-endpoint>] listen\n"
        "Defaults: cmd=%s event=%s\n",
        kDefaultCmdEndpoint, kDefaultEventEndpoint);
}

/* 组装请求信封 {"type":N,"payload":{...}}，payloadJson 为空则不带 payload */
std::string buildEnvelope(uint32_t type, const std::string& payloadJson) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", type);
    if (!payloadJson.empty()) {
        cJSON* payload = cJSON_Parse(payloadJson.c_str());
        if (!payload) {
            cJSON_Delete(root);
            return "";
        }
        cJSON_AddItemToObject(root, "payload", payload);
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(root);
    return result;
}

/* 发送请求并打印应答；返回进程退出码（应答 code!=0 时非零） */
int sendRequest(const std::string& endpoint, uint32_t type,
                const std::string& payloadJson) {
    std::string envelope = buildEnvelope(type, payloadJson);
    if (envelope.empty()) {
        std::fprintf(stderr, "error: invalid payload json\n");
        return 2;
    }

    libmq::ReqRepQueue client;
    if (!client.initializeAsClient(endpoint)) {
        std::fprintf(stderr, "error: connect %s failed (mediad not running?)\n",
                     endpoint.c_str());
        return 1;
    }

    libmq::MqResult result = client.request(envelope, kRequestTimeoutMs);
    if (!result.ok) {
        std::fprintf(stderr, "error: request failed: %s (timeout?)\n",
                     result.message.c_str());
        return 1;
    }

    /* 应答原样打印（美化输出便于人读） */
    cJSON* resp = cJSON_Parse(result.value.c_str());
    if (resp) {
        char* pretty = cJSON_Print(resp);
        std::printf("%s\n", pretty ? pretty : result.value.c_str());
        cJSON_free(pretty);
    } else {
        std::printf("%s\n", result.value.c_str());
    }

    int exitCode = 1;
    const cJSON* codeItem = resp ? cJSON_GetObjectItemCaseSensitive(resp, "code") : nullptr;
    if (cJSON_IsNumber(codeItem)) {
        exitCode = (codeItem->valueint == 0) ? 0 : 1;
    }
    cJSON_Delete(resp);
    return exitCode;
}

/* 订阅事件通道并持续打印（联调 FILE_READY / STATE_CHANGED / CONFIG_APPLIED） */
int listenEvents(const std::string& endpoint) {
    auto manager = libmq::MessageManager::builder()
                       .inprocEndpoint("inproc://mediactl_listen")
                       .ipcEndpoint(endpoint)
                       .asSubscriber()
                       .build();
    if (!manager) {
        std::fprintf(stderr, "error: build subscriber failed\n");
        return 1;
    }

    auto printer = [](const libmq::MessageHeader& header, const std::string& payload) {
        std::printf("[event %u] %s\n", header.type, payload.c_str());
        std::fflush(stdout);
    };
    manager->subscribeIpc(static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_FILE_READY), printer);
    manager->subscribeIpc(static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_STATE_CHANGED), printer);
    manager->subscribeIpc(static_cast<uint32_t>(libmq::IpcMessageType::MEDIA_CONFIG_APPLIED), printer);

    libmq::MqResult ret = manager->start();
    if (!ret.ok) {
        std::fprintf(stderr, "error: start subscriber failed: %s\n", ret.message.c_str());
        return 1;
    }

    std::printf("listening on %s (Ctrl+C to quit)...\n", endpoint.c_str());
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    manager->stop();
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string cmdEndpoint   = kDefaultCmdEndpoint;
    std::string eventEndpoint = kDefaultEventEndpoint;

    /* 解析可选端点参数 */
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (std::strcmp(argv[argi], "-e") == 0 && argi + 1 < argc) {
            cmdEndpoint = argv[argi + 1];
            argi += 2;
        } else if (std::strcmp(argv[argi], "-E") == 0 && argi + 1 < argc) {
            eventEndpoint = argv[argi + 1];
            argi += 2;
        } else {
            usage();
            return 2;
        }
    }
    if (argi >= argc) {
        usage();
        return 2;
    }

    const std::string cmd = argv[argi];

    if (cmd == "snapshot") {
        std::string payload;
        if (argi + 1 < argc) {
            payload = "{\"quality\":" + std::string(argv[argi + 1]) + "}";
        }
        return sendRequest(cmdEndpoint,
                           static_cast<uint32_t>(mediad::Command::MEDIA_SNAPSHOT), payload);
    }

    /* record 命令已废弃，录像由 triggers 系统控制 */

    if (cmd == "status") {
        return sendRequest(cmdEndpoint,
                           static_cast<uint32_t>(mediad::Command::MEDIA_GET_STATUS), "");
    }

    if (cmd == "config") {
        if (argi + 1 >= argc) {
            usage();
            return 2;
        }
        const std::string sub = argv[argi + 1];
        if (sub == "get") {
            return sendRequest(cmdEndpoint,
                               static_cast<uint32_t>(mediad::Command::MEDIA_GET_CONFIG), "");
        }
        if (sub == "set" && argi + 2 < argc) {
            /* 配置更新复用 lib-mq 内置 CONFIG_UPDATE(3000) */
            return sendRequest(cmdEndpoint,
                               static_cast<uint32_t>(libmq::IpcMessageType::CONFIG_UPDATE),
                               argv[argi + 2]);
        }
        usage();
        return 2;
    }

    if (cmd == "listen") {
        return listenEvents(eventEndpoint);
    }

    usage();
    return 2;
}
