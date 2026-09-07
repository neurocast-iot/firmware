/**
 * @file ConfigStore.cpp
 * @brief mediad 配置中心实现
 *
 * 解析规则（兜底原则）：每个键独立取值，缺键/类型不符一律保持当前值
 * （启动时即默认值），绝不因个别键损坏导致整个配置失效。
 */
#include "config/ConfigStore.h"

#include "nc/common/log_utils.h"
#include "nc/common/file_utils.h"

#include "cJSON.h"

#include <algorithm>
#include <cstdio>

namespace mediad {

namespace {

/* ---- 单键读取工具：类型正确才覆盖，否则保持原值 ---- */

void readBool(const cJSON* obj, const char* key, bool& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        out = cJSON_IsTrue(item);
    }
}

void readInt(const cJSON* obj, const char* key, int& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        out = item->valueint;
    }
}

void readString(const cJSON* obj, const char* key, std::string& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        out = item->valuestring;
    }
}

/* 千分比字段读取：只在类型正确时覆盖，越界值留给 normalizeOsd 统一钳位 */
void readPermille(const cJSON* obj, const char* key, int& out) {
    readInt(obj, key, out);
}

/* 千分比钳位到 [0,1000]：配置只存意图，离谱值拉到边界继续跑，
 * 不让单个坏值把整个 OSD 段打废 */
void clampPermille(int& v) {
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
}

/* 字号档位规范化：不在三档内的一律回落 medium，
 * 具体像素换算由 OsdService 按通道分辨率完成 */
void normalizeSize(std::string& size) {
    if (size != "small" && size != "medium" && size != "large") {
        size = "medium";
    }
}

/* OSD 段规范化（坐标钳位 + 枚举回落 + 坏元素剔除），
 * 两条解析路径（文件加载/配置下发）合并完都要走一遍。
 * 这里不做数量截断：能上屏几个由平台后端决定（anyka 固定 4，
 * 其他平台可能更多），超量截断和告警在 OsdService 分槽时做 */
void normalizeOsd(OsdCfg& osd) {
    auto& elems = osd.elements;
    /* 未知 type 直接剔除：留下的元素类型全都可识别，
     * 下游（OsdService）不用再判坏类型 */
    elems.erase(std::remove_if(elems.begin(), elems.end(),
        [](const OsdCfg::ElemCfg& e) {
            return e.type != "time" && e.type != "label" &&
                   e.type != "rect" && e.type != "circle" &&
                   e.type != "ellipse" && e.type != "polygon" &&
                   e.type != "bitmap";
        }),
        elems.end());
    for (auto& e : elems) {
        clampPermille(e.x);
        clampPermille(e.y);
        /* 透明度：没填的按类型补默认（矩形/多边形是遮盖块默认半透 20，
         * 其余不透明），填了的钳进 0~100；落盘后 -1 不再出现 */
        if (e.opacity < 0) {
            e.opacity = (e.type == "rect" || e.type == "polygon") ? 20 : 0;
        }
        if (e.opacity > 100) e.opacity = 100;
        if (e.type == "time" || e.type == "label") {
            normalizeSize(e.size);
            if (e.type == "time" &&
                e.format != "YYYY-MM-DD" && e.format != "MM-DD-YYYY" &&
                e.format != "Chinese") {
                e.format = "YYYY-MM-DD";
            }
        } else if (e.type == "polygon") {
            /* 顶点截断到 16 个：叠加区域内存有限，防恶意配置塞几千个点；
             * 逐点鉗进 [0,1000]，坏点拉到边界不剔除（点数比坐标重要） */
            if (e.points.size() > 16) {
                e.points.resize(16);
            }
            for (auto& p : e.points) {
                clampPermille(p.first);
                clampPermille(p.second);
            }
            if (e.color != "black" && e.color != "white" && e.color != "red" &&
                e.color != "green" && e.color != "blue" && e.color != "yellow") {
                e.color = "black";
            }
        } else if (e.type == "bitmap") {
            clampPermille(e.w);
            clampPermille(e.h);
        } else {
            clampPermille(e.w);
            clampPermille(e.h);
            clampPermille(e.r);
            if (e.color != "black" && e.color != "white" && e.color != "red" &&
                e.color != "green" && e.color != "blue" && e.color != "yellow") {
                e.color = "black";
            }
        }
    }
    /* 画不出来的元素提前剔除：多边形不足 3 个点、位图缺路径或尺寸，
     * 留下只会白白占服务层的创建失败日志 */
    elems.erase(std::remove_if(elems.begin(), elems.end(),
        [](const OsdCfg::ElemCfg& e) {
            if (e.type == "polygon" && e.points.size() < 3) return true;
            if (e.type == "bitmap" &&
                (e.imagePath.empty() || e.w <= 0 || e.h <= 0)) return true;
            return false;
        }),
        elems.end());
}

/* 单个元素 JSON 读取：各类型共用一组键，每个键独立读取，
 * 缺键保持默认值，不会因个别键缺失丢掉整个元素 */
void readOsdElem(const cJSON* item, OsdCfg::ElemCfg& out) {
    readString(item, "id", out.id);
    readString(item, "type", out.type);
    readBool(item, "enabled", out.enabled);
    readPermille(item, "x", out.x);
    readPermille(item, "y", out.y);
    /* 文本类字段 */
    readString(item, "text", out.text);
    readString(item, "size", out.size);
    readString(item, "format", out.format);
    readBool(item, "show_week", out.showWeek);
    /* 图形类字段 */
    readPermille(item, "w", out.w);
    readPermille(item, "h", out.h);
    readPermille(item, "r", out.r);
    readString(item, "color", out.color);
    readInt(item, "opacity", out.opacity);
    /* polygon 顶点：[[x,y],...] 千分比坐标，非数值/非二元组项跳过 */
    const cJSON* pts = cJSON_GetObjectItemCaseSensitive(item, "points");
    if (cJSON_IsArray(pts)) {
        const cJSON* p = nullptr;
        cJSON_ArrayForEach(p, pts) {
            if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) < 2) continue;
            const cJSON* px = cJSON_GetArrayItem(p, 0);
            const cJSON* py = cJSON_GetArrayItem(p, 1);
            if (cJSON_IsNumber(px) && cJSON_IsNumber(py)) {
                out.points.emplace_back(px->valueint, py->valueint);
            }
        }
    }
    /* bitmap 图片路径（设备上的绝对路径） */
    readString(item, "image_path", out.imagePath);
}

/* 旧版配置迁移：time/label/shapes 三段式 → 统一 elements 数组。
 * 设备上已落盘的旧格式配置升级后不能丢（丢了水印就没了），
 * 解析时发现没有 elements 键但有旧键时转换一次；
 * 落盘走 toJson 只写新格式，下次读盘就不再需要迁移。
 * 注意：必须在新格式解析之后调，旧键已读好的值才能进数组 */
void migrateOsdLegacy(const cJSON* osdJson, OsdCfg& osd) {
    /* 已有新格式就不迁移，防止旧键覆盖新配置 */
    if (!osd.elements.empty() ||
        cJSON_GetObjectItemCaseSensitive(osdJson, "elements")) {
        return;
    }
    /* 时间段 → time 元素（开关保留在元素的 enabled 里，统一由服务层判断） */
    const cJSON* timeObj = cJSON_GetObjectItemCaseSensitive(osdJson, "time");
    if (cJSON_IsObject(timeObj)) {
        OsdCfg::ElemCfg e;
        e.id = "time_main";
        e.type = "time";
        e.x = 12;               /* 旧版默认位置（同旧 TimeCfg 默认值） */
        e.y = 22;
        readOsdElem(timeObj, e);
        osd.elements.push_back(std::move(e));
    }
    /* 标签段 → label 元素（旧默认位置 12,89） */
    const cJSON* labelObj = cJSON_GetObjectItemCaseSensitive(osdJson, "label");
    if (cJSON_IsObject(labelObj)) {
        OsdCfg::ElemCfg e;
        e.id = "label_main";
        e.type = "label";
        e.enabled = false;      /* 旧 LabelCfg 默认关 */
        e.x = 12;
        e.y = 89;
        readOsdElem(labelObj, e);
        osd.elements.push_back(std::move(e));
    }
    /* shapes 数组 → rect/circle/ellipse 元素（type 由元素自己带） */
    const cJSON* shapes = cJSON_GetObjectItemCaseSensitive(osdJson, "shapes");
    if (cJSON_IsArray(shapes)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, shapes) {
            if (!cJSON_IsObject(item)) continue;
            OsdCfg::ElemCfg e;
            readOsdElem(item, e);
            osd.elements.push_back(std::move(e));
        }
    }
    NC_LOGI("ConfigStore: osd legacy time/label/shapes migrated to elements, count={}",
            osd.elements.size());
}

void readChannel(const cJSON* obj, const char* key, ChannelCfg& out) {
    const cJSON* ch = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsObject(ch)) {
        return;
    }
    readInt(ch, "width", out.width);
    readInt(ch, "height", out.height);
    readInt(ch, "fps", out.fps);
    readInt(ch, "bitrate_kbps", out.bitrateKbps);
    readString(ch, "codec", out.codec);
    readString(ch, "br_mode", out.brMode);
}

/* 通用范围检查：越界时把“哪个字段、当前值、允许范围”写进错误信息，
 * 供各配置段的 check() 成员函数复用 */
bool checkRange(const char* name, int value, int minV, int maxV, std::string& errMsg) {
    if (value < minV || value > maxV) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %d out of range [%d, %d]", name, value, minV, maxV);
        errMsg = buf;
        return false;
    }
    return true;
}

} // namespace

/* ---- MediadConfig 默认构造：初始化默认触发源配置 ----
 * 设备启动时如果没有配置文件，会使用这个默认配置
 * 默认：每 20 秒拍一张照片，全天生效（调试用，正式发布前改回 600） */
MediadConfig::MediadConfig() {
    TriggerCfg defaultTrigger;
    defaultTrigger.id = "timer_snapshot";
    defaultTrigger.type = "timer";
    defaultTrigger.enabled = true;
    defaultTrigger.intervalSec = 20;        // 20 秒（调试用）
    defaultTrigger.burstCount = 1;
    defaultTrigger.burstIntervalMs = 0;
    defaultTrigger.scheduleStart = "00:00";
    defaultTrigger.scheduleEnd = "23:59";
    defaultTrigger.scheduleDays = {0, 1, 2, 3, 4, 5, 6};  // 周日到周六
    triggers.push_back(std::move(defaultTrigger));
}

/* ---- 各配置段的校验实现（声明见 ConfigStore.h） ----
 *
 * 只拦两类会出大事的值：编码器硬件会硬拒绝的（服务起不来）、
 * 会烧 CPU/写爆磁盘的；其余值信任已有容错（OSD 有钳位、
 * 连不上有重连），不多拦 */

/* 规则来自编码器硬件的硬性要求
 * （libmpi_venc.so 内部检查，报错见 venc_check_and_format_param）：
 *   - 宽必须是 32 的倍数（硬件按 32 像素一块编码），高必须是 8 的倍数
 *   - fps、码率也有检查（fps error / max_kbps error）
 * 分辨率上限是传感器最大出图 2560x1440，fps 上限是传感器最大 30 帧，
 * 下限取工程保守值。
 *
 * 不在这里拦住的后果：非法值落盘后通道重建时编码器直接打开失败，
 * 录像/推流全部起不来，而且毒值已存进配置文件，重启进程也救不回来 */
bool ChannelCfg::check(const char* name, std::string& errMsg) const {
    char buf[160];
    /* 分辨率范围：宽 [160, 2560]，高 [128, 1440]
     * 下限放宽到 160x128：常见小分辨率如 320x176、160x120 都要能过
     * 上限是传感器最大出图 2560x1440 */
    if (width < 160 || width > 2560 ||
        height < 128 || height > 1440) {
        snprintf(buf, sizeof(buf), "%s resolution %dx%d out of range [160x128, 2560x1440]",
                 name, width, height);
        errMsg = buf;
        return false;
    }
    if (width % 32 != 0) {
        snprintf(buf, sizeof(buf), "%s width %d invalid: must be multiple of 32",
                 name, width);
        errMsg = buf;
        return false;
    }
    if (height % 8 != 0) {
        snprintf(buf, sizeof(buf), "%s height %d invalid: must be multiple of 8",
                 name, height);
        errMsg = buf;
        return false;
    }
    const std::string prefix = name;
    return checkRange((prefix + ".fps").c_str(), fps, 1, 30, errMsg) &&
           checkRange((prefix + ".bitrate_kbps").c_str(), bitrateKbps, 64, 16384, errMsg);
}

bool CameraCfg::check(std::string& errMsg) const {
    return main.check("camera.main", errMsg) &&
           sub.check("camera.sub", errMsg);
}

/* quality 范围校验 */
bool SnapshotCfg::check(std::string& errMsg) const {
    return checkRange("snapshot.quality", quality, 1, 100, errMsg);
}

/* 缩略图尺寸和质量校验 */
bool ThumbnailCfg::check(std::string& errMsg) const {
    return checkRange("thumbnail.width", width, 64, 640, errMsg) &&
           checkRange("thumbnail.height", height, 64, 480, errMsg) &&
           checkRange("thumbnail.quality", quality, 1, 100, errMsg);
}

/* segment_sec 下限 5：0 或负数转成无符号数后变天文数字，
 * 永不分段，一个文件无限涨到写爆磁盘 */
bool RecordCfg::check(std::string& errMsg) const {
    return checkRange("record.segment_sec", segmentSec, 5, 3600, errMsg);
}

bool MediadConfig::check(std::string& errMsg) const {
    return camera.check(errMsg) &&
           snapshot.check(errMsg) &&
           thumbnail.check(errMsg) &&
           record.check(errMsg);
}

/* ---- LiveCfg URL 模板替换 ---- */

namespace {
/* 字符串替换工具：把 str 里所有 oldSub 替换成 newSub */
std::string replaceAll(std::string str, const std::string& oldSub, const std::string& newSub) {
    size_t pos = 0;
    while ((pos = str.find(oldSub, pos)) != std::string::npos) {
        str.replace(pos, oldSub.length(), newSub);
        pos += newSub.length();
    }
    return str;
}
} // namespace

std::string LiveCfg::whipUrl(const std::string& devId, const std::string& accessToken) const {
    if (devId.empty()) return std::string();
    std::string url = replaceAll(srs.whipUrlTemplate, "{deviceUid}", devId);
    url = replaceAll(url, "{accessToken}", accessToken);
    return url;
}

bool ConfigStore::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_path = path;

    std::string content;
    if (!nc::common::ReadFile(path, content)) {
        NC_LOGW("ConfigStore: config file missing: {}, using defaults and writing template",
                path.c_str());
        persist();   /* 写出默认配置模板，方便现场修改 */
        return false;
    }

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) {
        NC_LOGE("ConfigStore: parse failed ({}), using defaults (file left untouched)",
                path.c_str());
        return false;
    }

    mergeFromJson(root, m_config);
    /* 文件里的越界值没法像 IPC 那样拒绝（服务必须起来），
     * 钳到边界继续跑；正常情况下文件是校验过才落盘的，这里只防手改文件 */
    if (m_config.snapshot.quality < 1)       m_config.snapshot.quality = 1;
    if (m_config.snapshot.quality > 100)     m_config.snapshot.quality = 100;
    if (m_config.thumbnail.width < 64)       m_config.thumbnail.width = 64;
    if (m_config.thumbnail.width > 640)      m_config.thumbnail.width = 640;
    if (m_config.thumbnail.height < 64)      m_config.thumbnail.height = 64;
    if (m_config.thumbnail.height > 480)     m_config.thumbnail.height = 480;
    if (m_config.thumbnail.quality < 1)      m_config.thumbnail.quality = 1;
    if (m_config.thumbnail.quality > 100)    m_config.thumbnail.quality = 100;
    if (m_config.record.segmentSec < 5)      m_config.record.segmentSec = 5;
    cJSON_Delete(root);
    NC_LOGI("ConfigStore: loaded {}", path.c_str());
    return true;
}

bool ConfigStore::applyUpdate(const std::string& patchJson, std::string& errMsg) {
    cJSON* root = cJSON_Parse(patchJson.c_str());
    if (!root) {
        errMsg = "invalid json";
        return false;
    }

    MediadConfig oldCfg;
    MediadConfig newCfg;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        oldCfg = m_config;
        newCfg = m_config;
        mergeFromJson(root, newCfg);
        /* 先校验合并后的配置再提交：不合规直接拒绝，
         * 不落盘、不通知任何服务，原配置保持不变。
         * 具体规则在各配置段自己的 check() 里 */
        if (!newCfg.check(errMsg)) {
            cJSON_Delete(root);
            NC_LOGW("ConfigStore: update rejected: {}", errMsg.c_str());
            return false;
        }
        m_config = newCfg;
        persist();
    }
    cJSON_Delete(root);

    NC_LOGI("ConfigStore: config updated via IPC, notifying {} listeners",
            m_listeners.size());
    /* 监听者在锁外通知：回调里可能反查 get()，避免死锁 */
    for (const auto& listener : m_listeners) {
        listener(oldCfg, newCfg);
    }
    return true;
}

MediadConfig ConfigStore::get() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void ConfigStore::addListener(ChangeListener listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners.push_back(std::move(listener));
}

void ConfigStore::mergeFromJson(const cJSON* root, MediadConfig& cfg) {
    /* 设备标识（顶层单键，应用级，由 IoT 进程下发） */
    readString(root, "device_id", cfg.deviceId);

    /* camera 段 */
    const cJSON* camera = cJSON_GetObjectItemCaseSensitive(root, "camera");
    if (cJSON_IsObject(camera)) {
        readBool(camera, "auto_start", cfg.camera.autoStart);
        readString(camera, "sensor_config", cfg.camera.sensorConfig);
        readChannel(camera, "main", cfg.camera.main);
        readChannel(camera, "sub", cfg.camera.sub);
    }

    /* osd 段（跨分辨率方案：位置千分比 + 字号档位，像素换算在 OsdService）。
     * 统一元素数组：出现即整体替换（空数组 = 清空全部水印） */
    const cJSON* osd = cJSON_GetObjectItemCaseSensitive(root, "osd");
    if (cJSON_IsObject(osd)) {
        readBool(osd, "enabled", cfg.osd.enabled);
        readString(osd, "font_file", cfg.osd.fontFile);
        const cJSON* elems = cJSON_GetObjectItemCaseSensitive(osd, "elements");
        if (cJSON_IsArray(elems)) {
            cfg.osd.elements.clear();
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, elems) {
                if (!cJSON_IsObject(item)) continue;
                OsdCfg::ElemCfg ec;
                readOsdElem(item, ec);
                cfg.osd.elements.push_back(std::move(ec));
            }
        } else {
            /* 没有 elements 键：旧版 time/label/shapes 配置就地转成元素数组，
             * 升级不丢水印；转完和新格式走同一套规范化 */
            migrateOsdLegacy(osd, cfg.osd);
        }
        normalizeOsd(cfg.osd);
    }

    /* snapshot 段 */
    const cJSON* snapshot = cJSON_GetObjectItemCaseSensitive(root, "snapshot");
    if (cJSON_IsObject(snapshot)) {
        readString(snapshot, "output_dir", cfg.snapshot.outputDir);
        readInt(snapshot, "quality", cfg.snapshot.quality);
    }

    /* thumbnail 段 */
    const cJSON* thumbnail = cJSON_GetObjectItemCaseSensitive(root, "thumbnail");
    if (cJSON_IsObject(thumbnail)) {
        readBool(thumbnail, "enabled", cfg.thumbnail.enabled);
        readInt(thumbnail, "width", cfg.thumbnail.width);
        readInt(thumbnail, "height", cfg.thumbnail.height);
        readInt(thumbnail, "quality", cfg.thumbnail.quality);
    }

    /* record 段 */
    const cJSON* record = cJSON_GetObjectItemCaseSensitive(root, "record");
    if (cJSON_IsObject(record)) {
        readInt(record, "segment_sec", cfg.record.segmentSec);
        readString(record, "output_dir", cfg.record.outputDir);
    }

    /* ipc 段（端点仅启动时生效，运行期改动下次重启才应用） */
    const cJSON* ipc = cJSON_GetObjectItemCaseSensitive(root, "ipc");
    if (cJSON_IsObject(ipc)) {
        readString(ipc, "iot_agent_endpoint", cfg.ipc.iotAgentEndpoint);
    }

    /* live 段（实时视频：P2P + SRS 降级链路，设备标识见顶层 device_id） */
    const cJSON* live = cJSON_GetObjectItemCaseSensitive(root, "live");
    if (cJSON_IsObject(live)) {
        readBool(live, "enabled", cfg.live.enabled);
        const cJSON* mqttSig = cJSON_GetObjectItemCaseSensitive(live, "mqtt_signaling");
        if (cJSON_IsObject(mqttSig)) {
            readString(mqttSig, "url", cfg.live.mqttSignaling.url);
            readString(mqttSig, "username", cfg.live.mqttSignaling.username);
            readString(mqttSig, "password", cfg.live.mqttSignaling.password);
        }
        const cJSON* ice = cJSON_GetObjectItemCaseSensitive(live, "ice");
        if (cJSON_IsObject(ice)) {
            readString(ice, "host", cfg.live.ice.host);
            readInt(ice, "port", cfg.live.ice.port);
            readString(ice, "username", cfg.live.ice.username);
            readString(ice, "password", cfg.live.ice.password);
        }
        const cJSON* srs = cJSON_GetObjectItemCaseSensitive(live, "srs");
        if (cJSON_IsObject(srs)) {
            readString(srs, "whip_url_template", cfg.live.srs.whipUrlTemplate);
        }
        readInt(live, "p2p_timeout_sec", cfg.live.p2pTimeoutSec);
        readInt(live, "relay_idle_sec", cfg.live.relayIdleSec);
    }

    /* triggers 段（触发源配置数组）
     * 云端可能传字符串形式的 JSON（ThingsBoard 共享属性不支持嵌套 JSON），
     * 也可能是直接的 JSON 数组，两种都要支持 */
    const cJSON* triggersRaw = cJSON_GetObjectItemCaseSensitive(root, "triggers");
    const cJSON* triggers = nullptr;
    cJSON* parsedTriggers = nullptr;
    if (cJSON_IsString(triggersRaw) && triggersRaw->valuestring[0] != '\0') {
        /* 字符串形式：先解析成 JSON */
        parsedTriggers = cJSON_Parse(triggersRaw->valuestring);
        triggers = parsedTriggers;
    } else {
        triggers = triggersRaw;
    }
    if (cJSON_IsArray(triggers) && cJSON_GetArraySize(triggers) > 0) {
        cfg.triggers.clear();
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, triggers) {
            if (!cJSON_IsObject(item)) continue;
            TriggerCfg tcfg;
            readString(item, "id", tcfg.id);
            readString(item, "type", tcfg.type);
            readBool(item, "enabled", tcfg.enabled);
            readInt(item, "priority", tcfg.priority);
            readInt(item, "interval_sec", tcfg.intervalSec);
            readInt(item, "burst_count", tcfg.burstCount);
            readInt(item, "burst_interval_ms", tcfg.burstIntervalMs);
            readBool(item, "all_day", tcfg.allDay);
            /* 时间表 */
            const cJSON* sched = cJSON_GetObjectItemCaseSensitive(item, "schedule");
            if (cJSON_IsObject(sched)) {
                readString(sched, "start_time", tcfg.scheduleStart);
                readString(sched, "end_time", tcfg.scheduleEnd);
                const cJSON* days = cJSON_GetObjectItemCaseSensitive(sched, "days");
                if (cJSON_IsArray(days)) {
                    tcfg.scheduleDays.clear();
                    const cJSON* d = nullptr;
                    cJSON_ArrayForEach(d, days) {
                        if (cJSON_IsNumber(d)) {
                            tcfg.scheduleDays.push_back(d->valueint);
                        }
                    }
                }
            }
            cfg.triggers.push_back(std::move(tcfg));
        }
    }
    /* 释放字符串形式解析出来的 JSON */
    if (parsedTriggers) cJSON_Delete(parsedTriggers);

    /* ---- live 段规范化（两条路径都要：live 段不走拒绝式校验，
     * 离谱值静默修正，连不上有重连兜底） ---- */
    if (cfg.live.ice.port < 1 || cfg.live.ice.port > 65535) cfg.live.ice.port = 3478;
    if (cfg.live.p2pTimeoutSec < 5)     cfg.live.p2pTimeoutSec = 5;
    /* 停推窗口必须大于观看端心跳周期（10s），否则正常心跳也会被判离开 */
    if (cfg.live.relayIdleSec < 15)     cfg.live.relayIdleSec = 15;
}

std::string ConfigStore::toJson() const {
    /* 注意：内部使用，调用路径已持锁（persist）或无需锁（只读拷贝后），
     * 这里直接读成员；对外语义见头文件 */
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "device_id", m_config.deviceId.c_str());

    cJSON* camera = cJSON_AddObjectToObject(root, "camera");
    cJSON_AddBoolToObject(camera, "auto_start", m_config.camera.autoStart);
    cJSON_AddStringToObject(camera, "sensor_config", m_config.camera.sensorConfig.c_str());
    cJSON* mainCh = cJSON_AddObjectToObject(camera, "main");
    cJSON_AddNumberToObject(mainCh, "width", m_config.camera.main.width);
    cJSON_AddNumberToObject(mainCh, "height", m_config.camera.main.height);
    cJSON_AddNumberToObject(mainCh, "fps", m_config.camera.main.fps);
    cJSON_AddNumberToObject(mainCh, "bitrate_kbps", m_config.camera.main.bitrateKbps);
    cJSON_AddStringToObject(mainCh, "codec", m_config.camera.main.codec.c_str());
    cJSON* subCh = cJSON_AddObjectToObject(camera, "sub");
    cJSON_AddNumberToObject(subCh, "width", m_config.camera.sub.width);
    cJSON_AddNumberToObject(subCh, "height", m_config.camera.sub.height);
    cJSON_AddNumberToObject(subCh, "fps", m_config.camera.sub.fps);
    cJSON_AddNumberToObject(subCh, "bitrate_kbps", m_config.camera.sub.bitrateKbps);
    cJSON_AddStringToObject(subCh, "codec", m_config.camera.sub.codec.c_str());

    cJSON* osd = cJSON_AddObjectToObject(root, "osd");
    cJSON_AddBoolToObject(osd, "enabled", m_config.osd.enabled);
    cJSON_AddStringToObject(osd, "font_file", m_config.osd.fontFile.c_str());
    /* 统一元素数组：只写新格式，旧 time/label/shapes 键不再落盘，
     * 各类型只写自己用得着的字段，配置紧凑不掺无关键 */
    cJSON* elems = cJSON_AddArrayToObject(osd, "elements");
    for (const auto& e : m_config.osd.elements) {
        cJSON* s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "id", e.id.c_str());
        cJSON_AddStringToObject(s, "type", e.type.c_str());
        cJSON_AddBoolToObject(s, "enabled", e.enabled);
        cJSON_AddNumberToObject(s, "x", e.x);
        cJSON_AddNumberToObject(s, "y", e.y);
        /* 透明度规范化后必有确定值（0~100），各类型都写 */
        cJSON_AddNumberToObject(s, "opacity", e.opacity);
        if (e.type == "time" || e.type == "label") {
            if (e.type == "label") {
                cJSON_AddStringToObject(s, "text", e.text.c_str());
            } else {
                cJSON_AddStringToObject(s, "format", e.format.c_str());
                cJSON_AddBoolToObject(s, "show_week", e.showWeek);
            }
            cJSON_AddStringToObject(s, "size", e.size.c_str());
        } else if (e.type == "polygon") {
            cJSON* pts = cJSON_AddArrayToObject(s, "points");
            for (const auto& p : e.points) {
                cJSON* pt = cJSON_CreateArray();
                cJSON_AddItemToArray(pt, cJSON_CreateNumber(p.first));
                cJSON_AddItemToArray(pt, cJSON_CreateNumber(p.second));
                cJSON_AddItemToArray(pts, pt);
            }
            cJSON_AddStringToObject(s, "color", e.color.c_str());
        } else if (e.type == "bitmap") {
            cJSON_AddNumberToObject(s, "w", e.w);
            cJSON_AddNumberToObject(s, "h", e.h);
            cJSON_AddStringToObject(s, "image_path", e.imagePath.c_str());
        } else {
            cJSON_AddNumberToObject(s, "w", e.w);
            cJSON_AddNumberToObject(s, "h", e.h);
            cJSON_AddNumberToObject(s, "r", e.r);
            cJSON_AddStringToObject(s, "color", e.color.c_str());
        }
        cJSON_AddItemToArray(elems, s);
    }

    cJSON* snapshot = cJSON_AddObjectToObject(root, "snapshot");
    cJSON_AddStringToObject(snapshot, "output_dir", m_config.snapshot.outputDir.c_str());
    cJSON_AddNumberToObject(snapshot, "quality", m_config.snapshot.quality);

    cJSON* thumbnail = cJSON_AddObjectToObject(root, "thumbnail");
    cJSON_AddBoolToObject(thumbnail, "enabled", m_config.thumbnail.enabled);
    cJSON_AddNumberToObject(thumbnail, "width", m_config.thumbnail.width);
    cJSON_AddNumberToObject(thumbnail, "height", m_config.thumbnail.height);
    cJSON_AddNumberToObject(thumbnail, "quality", m_config.thumbnail.quality);

    cJSON* record = cJSON_AddObjectToObject(root, "record");
    cJSON_AddNumberToObject(record, "segment_sec", m_config.record.segmentSec);
    cJSON_AddStringToObject(record, "output_dir", m_config.record.outputDir.c_str());

    cJSON* ipc = cJSON_AddObjectToObject(root, "ipc");
    cJSON_AddStringToObject(ipc, "iot_agent_endpoint", m_config.ipc.iotAgentEndpoint.c_str());

    cJSON* live = cJSON_AddObjectToObject(root, "live");
    cJSON_AddBoolToObject(live, "enabled", m_config.live.enabled);
    cJSON* mqttSig = cJSON_AddObjectToObject(live, "mqtt_signaling");
    cJSON_AddStringToObject(mqttSig, "url", m_config.live.mqttSignaling.url.c_str());
    cJSON_AddStringToObject(mqttSig, "username", m_config.live.mqttSignaling.username.c_str());
    cJSON_AddStringToObject(mqttSig, "password", m_config.live.mqttSignaling.password.c_str());
    cJSON* ice = cJSON_AddObjectToObject(live, "ice");
    cJSON_AddStringToObject(ice, "host", m_config.live.ice.host.c_str());
    cJSON_AddNumberToObject(ice, "port", m_config.live.ice.port);
    cJSON_AddStringToObject(ice, "username", m_config.live.ice.username.c_str());
    cJSON_AddStringToObject(ice, "password", m_config.live.ice.password.c_str());
    cJSON* srs = cJSON_AddObjectToObject(live, "srs");
    cJSON_AddStringToObject(srs, "whip_url_template", m_config.live.srs.whipUrlTemplate.c_str());
    cJSON_AddNumberToObject(live, "p2p_timeout_sec", m_config.live.p2pTimeoutSec);
    cJSON_AddNumberToObject(live, "relay_idle_sec", m_config.live.relayIdleSec);

    /* triggers 段 */
    cJSON* triggers = cJSON_AddArrayToObject(root, "triggers");
    for (const auto& tcfg : m_config.triggers) {
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "id", tcfg.id.c_str());
        cJSON_AddStringToObject(t, "type", tcfg.type.c_str());
        cJSON_AddBoolToObject(t, "enabled", tcfg.enabled);
        cJSON_AddNumberToObject(t, "priority", tcfg.priority);
        cJSON_AddNumberToObject(t, "interval_sec", tcfg.intervalSec);
        cJSON_AddNumberToObject(t, "burst_count", tcfg.burstCount);
        cJSON_AddNumberToObject(t, "burst_interval_ms", tcfg.burstIntervalMs);
        cJSON_AddBoolToObject(t, "all_day", tcfg.allDay);
        if (!tcfg.scheduleStart.empty() || !tcfg.scheduleEnd.empty()) {
            cJSON* sched = cJSON_AddObjectToObject(t, "schedule");
            cJSON_AddStringToObject(sched, "start_time", tcfg.scheduleStart.c_str());
            cJSON_AddStringToObject(sched, "end_time", tcfg.scheduleEnd.c_str());
            cJSON* days = cJSON_AddArrayToObject(sched, "days");
            for (int d : tcfg.scheduleDays) {
                cJSON_AddItemToArray(days, cJSON_CreateNumber(d));
            }
        }
        cJSON_AddItemToArray(triggers, t);
    }

    char* printed = cJSON_Print(root);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return result;
}

void ConfigStore::persist() {
    if (m_path.empty()) {
        return;
    }
    /* 父目录不存在先创建（如 /etc/config 首次启动），否则原子写入必失败 */
    size_t slash = m_path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        nc::common::MakeDirs(m_path.substr(0, slash));
    }
    if (!nc::common::WriteFileAtomic(m_path, toJson())) {
        NC_LOGE("ConfigStore: persist failed: {}", m_path.c_str());
    }
}

} // namespace mediad
