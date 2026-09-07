/**
 * @file tb_provision.cpp
 * @brief ThingsBoard provision 实现
 *
 * 从 thingsboard_platform.cpp 的 acquireToken() 里拆出来的。
 * 原来那段代码有 130+ 行，混在 ThingsBoardPlatform 类里，
 * 现在拆成独立函数，职责更清晰。
 */
#include "tb_provision.h"
#include "token_store.h"
#include "tb_topics.h"

#include "nc/common/log_utils.h"
#include "nc/mqtt/mqtt_client.h"
#include "cJSON.h"

#include <condition_variable>
#include <chrono>
#include <mutex>

namespace iot_agent {

/* ---- 工具：从 cJSON 里读字符串 ---- */
static void readStr(const cJSON* root, const char* key, std::string& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) out = item->valuestring;
}

/**
 * TB provision 实现
 *
 * 流程：
 *   1) 先查本地 token 文件（/data/iot_agent/token.json）
 *      有就直接用，不再走网络（TB 的 token 不会过期）
 *   2) 没有 → 起临时 MQTT 客户端走 provision：
 *      - username = "provision"，password 空（TB provision 的规矩）
 *      - 订阅 /provision/response
 *      - 发布 /provision/request 带 deviceKey + deviceSecret
 *      - 收到响应 → 提取 credentialsValue 就是 token
 *      - 存到本地文件 → 关闭临时客户端
 */
bool tbProvision(const std::string& cfgJson,
                 const std::string& deviceId,
                 std::string& tokenOut) {
    cJSON* cfg = cJSON_Parse(cfgJson.c_str());
    if (!cfg) {
        NC_LOGE("[TB] iot_agent config parse failed, skip provision");
        return false;
    }

    /* token 文件路径（硬编码 /data/iot_agent/token.json，嵌入式设备上 /data/ 可读写） */
    std::string tokenPath = "/data/iot_agent/token.json";

    /* 1) 先查本地：有就直接用，不再走网络 */
    if (TokenStore::load(tokenPath, tokenOut)) {
        NC_LOGI("[TB] token loaded from local file: {} (len={})",
                tokenPath.c_str(), tokenOut.size());
        cJSON_Delete(cfg);
        return true;
    }

    /* 2) 本地没有 → 读 provision 配置 */
    std::string url = "ssl://mqtt.example.com:8883";  /* 默认值 */
    std::string deviceKey;
    std::string deviceSecret;
    std::string deviceName;
    /* 解析统一格式的 MQTT 配置 */
    const cJSON* mqttObj = cJSON_GetObjectItemCaseSensitive(cfg, "mqtt");
    if (cJSON_IsObject(mqttObj)) {
        const cJSON* urlItem = cJSON_GetObjectItemCaseSensitive(mqttObj, "url");
        if (cJSON_IsString(urlItem) && urlItem->valuestring) {
            url = urlItem->valuestring;
        }
    }
    readStr(cfg, "provision_device_key", deviceKey);
    readStr(cfg, "provision_device_secret", deviceSecret);
    readStr(cfg, "device_name", deviceName);
    cJSON_Delete(cfg);

    if (url.empty() || deviceKey.empty() || deviceSecret.empty()) {
        NC_LOGE("[TB] provision config incomplete: url={} key_len={} secret_len={}",
                url.c_str(), deviceKey.size(), deviceSecret.size());
        return false;
    }
    if (deviceName.empty()) deviceName = deviceId;  /* 默认用设备 ID 当 deviceName */

    /* 3) 起临时 MQTT 客户端走 provision */
    nc::mqtt::MqttConfig mqttCfg;
    mqttCfg.address = url;
    mqttCfg.clientId = deviceName + "_provision";
    mqttCfg.username = "provision";
    mqttCfg.password = "";
    mqttCfg.autoReconnect = false;
    mqttCfg.keepAliveInterval = 20;
    mqttCfg.connectTimeoutMs = 10000;
    mqttCfg.defaultQos = 1;
    mqttCfg.topics = {tb::TOPIC_PROVISION_RESPONSE};
    /* 根据 URL 前缀判断是否启用 SSL */
    if (url.find("ssl://") == 0 || url.find("tls://") == 0) {
        mqttCfg.enableSsl = true;
        mqttCfg.verifyServerCert = false;
    }

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool success = false;

    auto client = new nc::mqtt::MqttClient(mqttCfg);

    client->setMessageCallback(
        [&, tokenPath, deviceName](const std::string& topic, const std::string& payload) {
            if (topic != tb::TOPIC_PROVISION_RESPONSE) return;
            NC_LOGI("[TB] provision response: {}", payload.c_str());

            cJSON* root = cJSON_Parse(payload.c_str());
            if (!root) { cv.notify_all(); return; }

            cJSON* status = cJSON_GetObjectItem(root, "status");
            if (!cJSON_IsString(status) || std::string(status->valuestring) != "SUCCESS") {
                NC_LOGE("[TB] provision status != SUCCESS");
                cJSON_Delete(root);
                cv.notify_all();
                return;
            }
            cJSON* cred = cJSON_GetObjectItem(root, "credentialsValue");
            if (cJSON_IsString(cred) && cred->valuestring) {
                std::string token = cred->valuestring;
                if (TokenStore::save(tokenPath, token)) {
                    tokenOut = token;
                    success = true;
                    NC_LOGI("[TB] provision success, token saved to {}", tokenPath.c_str());
                } else {
                    NC_LOGE("[TB] provision success but save token failed");
                }
            }
            cJSON_Delete(root);
            done = true;
            cv.notify_all();
        });

    client->setStateCallback(
        [&, deviceKey, deviceSecret, deviceName](nc::mqtt::MqttState state, const std::string&) {
            if (state != nc::mqtt::MqttState::Connected) return;
            NC_LOGI("[TB] provision MQTT connected, sending request");

            cJSON* req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "provisionDeviceKey", deviceKey.c_str());
            cJSON_AddStringToObject(req, "provisionDeviceSecret", deviceSecret.c_str());
            if (!deviceName.empty()) {
                cJSON_AddStringToObject(req, "deviceName", deviceName.c_str());
            }
            char* jsonStr = cJSON_PrintUnformatted(req);
            std::string payload = jsonStr ? jsonStr : "";
            free(jsonStr);
            cJSON_Delete(req);

            if (!client->publish(tb::TOPIC_PROVISION_REQUEST, payload, 1)) {
                NC_LOGE("[TB] publish /provision/request failed");
                done = true;
                cv.notify_all();
            }
        });

    if (!client->start()) {
        NC_LOGE("[TB] provision MQTT start failed");
        delete client;
        return false;
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(15), [&] { return done; });
    }

    client->stop();
    delete client;
    return success;
}

} // namespace iot_agent
