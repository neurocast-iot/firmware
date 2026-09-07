/**
 * @file tb_adapter.cpp
 * @brief ThingsBoard 适配器实现
 *
 * 从旧 thingsboard_platform.cpp 搬过来的逻辑，适配新 CloudAdapter 接口：
 *   - configureMqtt：用 token 拼 TB 的 MQTT 登录参数
 *   - parseMessage：解析 TB 消息 → DeviceData（替代旧的 handleMessage + 回调）
 *   - requestAttributes / publish：直接传 MqttClient&，不再存指针
 *
 * provision 逻辑已拆到 tb_provision.cpp，这里 authenticate() 只调函数。
 */
#include "tb_adapter.h"
#include "tb_provision.h"
#include "tb_topics.h"

#include "nc/common/log_utils.h"
#include "nc/mqtt/mqtt_client.h"
#include "cJSON.h"

#include <cstring>

namespace iot_agent {

/* ---- 工具：从 cJSON 里读字符串 / 整数 ---- */
static void readStr(const cJSON* root, const char* key, std::string& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) out = item->valuestring;
}

/**
 * TB 认证：走 provision 拿 accessToken
 *
 * 直接委托给 tb_provision.cpp 的 tbProvision()，
 * 那里已经封装了"先查本地 → 没有就走网络"的完整流程。
 */
bool TBAdapter::authenticate(const std::string& cfgJson,
                              const std::string& deviceId,
                              std::string& credentialOut) {
    return tbProvision(cfgJson, deviceId, credentialOut);
}

/**
 * 用 token 配 TB 的 MQTT 登录参数
 *
 * TB 的规矩：username = accessToken，password 空。
 * clientId 用 deviceName + "_live"，方便在 TB 设备列表里辨认。
 *
 * 配置格式（统一 MQTT 配置）：
 *   {"mqtt": {"url": "ssl://host:port", "username": "", "password": ""}}
 * TB 特殊：username 被 accessToken 覆盖，config 里的 username/password 不生效
 */
void TBAdapter::configureMqtt(nc::mqtt::MqttConfig& mqttCfg,
                               const std::string& credential,
                               const std::string& cfgJson,
                               const std::string& deviceId) {
    cJSON* cfg = cJSON_Parse(cfgJson.c_str());
    std::string deviceName = deviceId;  /* 默认用设备 ID，配置里可以覆盖 */
    if (cfg) {
        readStr(cfg, "device_name", deviceName);
        cJSON_Delete(cfg);
    }

    /* 解析统一格式的 MQTT 配置 */
    std::string url = "ssl://mqtt.example.com:8883";  /* 默认值 */
    cfg = cJSON_Parse(cfgJson.c_str());
    if (cfg) {
        const cJSON* mqttObj = cJSON_GetObjectItemCaseSensitive(cfg, "mqtt");
        if (cJSON_IsObject(mqttObj)) {
            const cJSON* urlItem = cJSON_GetObjectItemCaseSensitive(mqttObj, "url");
            if (cJSON_IsString(urlItem) && urlItem->valuestring) {
                url = urlItem->valuestring;
            }
        }
        cJSON_Delete(cfg);
    }

    mqttCfg.address = url;
    mqttCfg.clientId = deviceName + "_live";
    mqttCfg.username = credential;    /* TB 用 accessToken 当 username */
    mqttCfg.password = "";            /* TB 的 accessToken 不需要 password */
    mqttCfg.keepAliveInterval = 60;
    mqttCfg.autoReconnect = true;
    mqttCfg.initialReconnectIntervalMs = 2000;
    mqttCfg.maxReconnectIntervalMs = 30000;
    mqttCfg.connectTimeoutMs = 10000;
    mqttCfg.defaultQos = 1;
    /* 根据 URL 前缀判断是否启用 SSL */
    if (url.find("ssl://") == 0 || url.find("tls://") == 0) {
        mqttCfg.enableSsl = true;
        mqttCfg.verifyServerCert = false;
    }
}

/** TB 需要订阅的主题列表，直接从 tb_topics.h 取 */
std::vector<std::string> TBAdapter::subscribeTopics() const {
    return tb::subscribeTopics();
}

/** TB 的遥测/属性上报主题（平台细节，只在这个文件里出现） */
std::string TBAdapter::telemetryTopic() const {
    return tb::TOPIC_TELEMETRY;
}

std::string TBAdapter::attributesTopic() const {
    return tb::TOPIC_ATTRIBUTES;
}

/**
 * 解析 TB 推送的 MQTT 消息 → DeviceData 列表
 *
 * TB 三种消息：
 *   - v1/devices/me/attributes           → Attributes（配置推送）
 *   - v1/devices/me/attributes/response/+ → AttributesResponse（requestAttributes 的回复）
 *   - v1/devices/me/rpc/request/+        → RpcRequest（RPC 指令）
 *
 * 注意 attributes 推送有时带 "shared" 包裹节点，有时不带，两种都兼容：
 *   - 有 shared 节点 → 取 shared 里的内容
 *   - 没有 → 直接用 root
 */
std::vector<DeviceData> TBAdapter::parseMessage(const std::string& topic,
                                                  const std::string& payload) {
    std::vector<DeviceData> result;

    /* ---- 配置推送 / 属性响应 ---- */
    /*
     * 匹配三种主题：
     *   v1/devices/me/attributes              ← TB 主动推送的属性变更
     *   v1/devices/me/attributes/request/{id} ← 我们发出去的请求（一般不会收到）
     *   v1/devices/me/attributes/response/{id} ← TB 对我们请求的回复
     */
    const std::string attrPrefix = "v1/devices/me/attributes";
    if (topic == tb::TOPIC_ATTRIBUTES ||
        topic.find(attrPrefix + "/") == 0) {

        /* 判断是"推送"还是"响应"：主题里有 "response/" 就是响应 */
        DeviceData::Type dtype = DeviceData::Type::Attributes;
        if (topic.find("attributes/response/") != std::string::npos) {
            dtype = DeviceData::Type::AttributesResponse;
        }

        /* 兼容 TB 的 "shared" 包裹节点 */
        cJSON* root = cJSON_Parse(payload.c_str());
        if (!root) {
            NC_LOGW("[TB] attributes JSON parse failed");
            return result;
        }

        /* 找到实际的数据节点：有 "shared" 就用 shared，没有就用 root */
        cJSON* dataNode = cJSON_GetObjectItem(root, "shared");
        if (!dataNode || !cJSON_IsObject(dataNode)) {
            dataNode = root;
        }

        /* 提取要保存的 JSON（去掉 shared 包裹） */
        std::string jsonToSave;
        if (dataNode != root) {
            /* 有 shared 包裹，只保存 shared 里面的内容 */
            char* s = cJSON_PrintUnformatted(dataNode);
            jsonToSave = s ? s : "";
            if (s) free(s);
        } else {
            /* 没有 shared 包裹，保存整个 payload */
            jsonToSave = payload;
        }
        cJSON_Delete(root);

        DeviceData data;
        data.type = dtype;
        data.rawJson = std::move(jsonToSave);
        data.source = name();
        result.push_back(std::move(data));

        NC_LOGI("[TB] attributes: type={} len={}",
                static_cast<int>(dtype), payload.size());
        return result;
    }

    /* ---- RPC 指令 ---- */
    const std::string rpcPrefix = "v1/devices/me/rpc/request/";
    if (topic.find(rpcPrefix) == 0) {
        std::string requestId = topic.substr(rpcPrefix.size());

        cJSON* root = cJSON_Parse(payload.c_str());
        if (!root) {
            NC_LOGW("[TB] rpc parse failed: {}", payload.c_str());
            return result;
        }

        DeviceData data;
        data.type = DeviceData::Type::RpcRequest;
        data.source = name();
        data.requestId = std::move(requestId);

        cJSON* method = cJSON_GetObjectItem(root, "method");
        if (cJSON_IsString(method) && method->valuestring) {
            data.method = method->valuestring;
        }

        cJSON* params = cJSON_GetObjectItem(root, "params");
        if (params) {
            char* pstr = cJSON_PrintUnformatted(params);
            if (pstr) {
                data.paramsJson = pstr;
                free(pstr);
            }
        }

        NC_LOGI("[TB] rpc: id={} method={} params={}",
                data.requestId.c_str(), data.method.c_str(), data.paramsJson.c_str());

        cJSON_Delete(root);
        result.push_back(std::move(data));
        return result;
    }

    NC_LOGW("[TB] unhandled topic: {}", topic.c_str());
    return result;
}

/**
 * 主动请求服务器下发共享属性
 *
 * 发布到 v1/devices/me/attributes/request/{id}，
 * 服务器会响应到 v1/devices/me/attributes/response/{id}，
 * 响应由 parseMessage() 接收解析。
 */
bool TBAdapter::requestAttributes(nc::mqtt::MqttClient& client) {
    int id = ++m_requestId;
    std::string topic = std::string(tb::TOPIC_ATTR_REQUEST_FMT) + std::to_string(id);

    /*
     * 请求共享属性：必须把 key 名列举出来，逗号分隔。
     * TB 不支持空字符串请求全部（官方 GitHub issue #5906 确认过）。
     * key 列表和原始 iot_live 的 config_shared_keys.h 保持一致。
     */
    std::string payload = "{\"sharedKeys\": \""
        "last_updated_time,"
        "mqtt_url,"  // MQTT 地址
        "upload_file_base_url,upload_file_api_key,"  //上传文件
        "mqtt_signaling_url,"   // mqtt 信令地址
        "push_camera_stream_url," //推流地址
        "main_codec,main_frame_rate,main_bitrate,main_br_mode,main_resolution_width,main_resolution_height," //主通道
        "sub_codec,sub_frame_rate,sub_bitrate,sub_br_mode,sub_resolution_width,sub_resolution_height," //子通道
        "osd_enable,osd_elements,triggers," //osd 功能（坐标用千分比，无基准分辨率；
                                             //旧方案的 osd_base_resolution_* 已废弃不再请求）
        "record_segment_sec," //录影
        "snapshot_quality," //拍照
        "fw_title,fw_version,fw_url,fw_checksum,fw_size,fw_tag," //固件
        "sw_title,sw_version,sw_url,sw_checksum,sw_size,sw_tag"  //软件
        "\"}";

    if (client.publish(topic, payload, 1)) {
        NC_LOGI("[TB] requestAttributes sent: id={}", id);
        return true;
    }
    NC_LOGE("[TB] requestAttributes failed: id={}", id);
    return false;
}

/**
 * 发送 MQTT 消息
 *
 * 返回值语义和 MqttClient::publish 一致：
 *   1 = 已发送, 2 = 已缓存待重发（断线中）, 0 = 失败
 */
int TBAdapter::publish(nc::mqtt::MqttClient& client,
                        const std::string& topic,
                        const std::string& payload,
                        int qos) {
    return client.publish(topic, payload, qos);
}

} // namespace iot_agent
