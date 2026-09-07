/**
 * @file ConfigStore.h
 * @brief mediad 配置中心
 *
 * 配置一条路径两个入口：
 *   - 启动时 load() 读取 mediad.json
 *   - 运行期 applyUpdate() 接收 IPC CONFIG_UPDATE 下发的局部 JSON
 * 两个入口最终都走同一条解析路径（每键默认值兜底：缺键/类型错都回落默认），
 * applyUpdate 合并成功后原子回写配置文件（断电安全），并通知变更监听者。
 *
 * 线程模型：load() 在主线程启动阶段调用；applyUpdate() 在 IPC 命令线程
 * 串行调用（REQ/REP 天然串行）；get() 返回拷贝可在任意线程使用。
 */
#pragma once

#include "camera/camera_config.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

/* cJSON 前向声明，避免头文件外泄 C 库依赖 */
struct cJSON;

namespace mediad {

/** 单视频通道配置（与 nc::camera ChannelConfig 对应字段） */
struct ChannelCfg {
    int width;
    int height;
    int fps;
    int bitrateKbps;
    std::string codec = "h264";   ///< 编码格式："h264" 或 "h265"
    std::string brMode  = "vbr";  ///< 码率控制模式："vbr" 或 "cbr"

    /** 校验是否满足编码器硬件要求（宽 32 对齐/高 8 对齐/范围）；
     *  name 是错误信息里的字段前缀（如 "camera.main"） */
    bool check(const char* name, std::string& errMsg) const;
};

/** camera::ChannelConfig（硬件实际生效值）→ mediad::ChannelCfg（OSD 钳位基准）
 *  两层域的桥接：硬件层 → 配置层 */
inline ChannelCfg toChannelCfg(const camera::ChannelConfig& c) {
    ChannelCfg out;
    out.width       = c.width;
    out.height      = c.height;
    out.fps         = c.fps;
    out.bitrateKbps = c.bitrate;
    return out;
}

/** 相机配置段 */
struct CameraCfg {
    bool        autoStart    = true;                 ///< 启动后自动开流
    std::string sensorConfig = "/etc/isp_sensor.conf";
    ChannelCfg  main = {1920, 1080, 25, 2048};       ///< 主通道（录像/推流）
    ChannelCfg  sub  = {640, 480, 15, 512};          ///< 子通道（拍照隔离通道）

    /** 校验主/子通道参数（规则见 ChannelCfg::check） */
    bool check(std::string& errMsg) const;
};

/** mediad::CameraCfg（配置层）→ camera::CameraConfig（硬件层）
 *  两层域的桥接：配置层 → 硬件层，与 toChannelCfg 方向相反 */
inline camera::CameraConfig toCameraConfig(const CameraCfg& cfg) {
    camera::ChannelConfig mainChn;
    mainChn.width   = static_cast<uint16_t>(cfg.main.width);
    mainChn.height  = static_cast<uint16_t>(cfg.main.height);
    mainChn.fps     = static_cast<uint8_t>(cfg.main.fps);
    mainChn.bitrate = static_cast<uint16_t>(cfg.main.bitrateKbps);
    mainChn.codec   = camera::parseCodecType(cfg.main.codec);
    mainChn.brMode  = camera::parseBitrateMode(cfg.main.brMode);

    camera::ChannelConfig subChn;
    subChn.width   = static_cast<uint16_t>(cfg.sub.width);
    subChn.height  = static_cast<uint16_t>(cfg.sub.height);
    subChn.fps     = static_cast<uint8_t>(cfg.sub.fps);
    subChn.bitrate = static_cast<uint16_t>(cfg.sub.bitrateKbps);
    subChn.codec   = camera::parseCodecType(cfg.sub.codec);
    subChn.brMode  = camera::parseBitrateMode(cfg.sub.brMode);

    return camera::CameraConfig::builder()
        .deviceId(0)
        .sensorConfig(cfg.sensorConfig)
        .mainChannel(mainChn)
        .subChannel(subChn)
        .build();
}

/** OSD 水印配置段（跨分辨率方案，详见 docs/osd-水印配置与多平台架构方案.md）
 *
 * 配置只存"意图"，不存像素：
 *   - 位置 x/y、图形尺寸 w/h/r 一律用千分比（0~1000，占画面宽/高的比例）
 *   - 字号用档位枚举（small/medium/large），设备按各通道实际分辨率
 *     换算成像素并对齐 16 的倍数（点阵字库只支持整倍放大）
 * 换算和钳位全部发生在 OsdService，前端/云端不参与像素计算。
 *
 * 统一元素数组：时间/文本/图形全是同一种"水印元素"，接什么类型、
 * 接几个由数组说了算（时间/文本不再限单个）；每通道实际能上屏几个由设备问平台后端，
 * anyka 芯片固定 4 个，其他平台可以更多，超量的由 OsdService 截断并告警 */
struct OsdCfg {
    bool        enabled  = true;
    /* 点阵字库路径：OsdService.start 时交给引擎，后端每次画字符串前自动重设 */
    std::string fontFile = "/usr/share/fonts/osd_font_16.bin";

    /* 单个水印元素：各类型共用一个结构，各取自己的字段，
     * 用不着的字段保持默认值不参与比较 */
    struct ElemCfg {
        std::string id;                            ///< 元素唯一标识（云端管理用）
        std::string type = "time";                 ///< time / label / rect / circle / ellipse / polygon / bitmap
        bool        enabled = true;                ///< 单元素开关（保留在屏上但不画也可用此关）
        int         x = 0;                         ///< 千分比：文本类=左上角，图形类=中心点（圆/椭圆）或左上角（矩形/位图）
        int         y = 0;                         ///< 千分比（占画面高度）
        /* ---- 文本类（time/label）字段 ---- */
        std::string text;                          ///< label 的固定文本内容（如设备名）
        std::string size     = "medium";           ///< 字号档位：small/medium/large
        std::string format   = "YYYY-MM-DD";       ///< time 专用：YYYY-MM-DD / MM-DD-YYYY / Chinese
        bool        showWeek = false;              ///< time 专用：时间后是否显示星期（如"周四"）
        /* ---- 图形类（rect/circle/ellipse）字段 ---- */
        int w = 0;                                 ///< rect 宽 / ellipse 横向直径 / bitmap 目标显示宽（千分比）
        int h = 0;                                 ///< rect 高 / ellipse 纵向直径 / bitmap 目标显示高（千分比）
        int r = 0;                                 ///< circle 半径（千分比，占画面高度）
        std::string color = "black";               ///< black/white/red/green/blue/yellow（polygon 也用它当填充色）
        int opacity = -1;                          ///< 透明度 0~100（0=不透明）；-1=没填，
                                                   ///< 规范化时按类型补默认（遮盖类 20，其余 0）
        /* ---- polygon/bitmap 专用字段 ---- */
        std::vector<std::pair<int, int>> points;   ///< polygon 顶点（千分比坐标，至少 3 个，配置层截断到 16 个）
        std::string imagePath;                     ///< bitmap 的 BMP 文件路径（设备上绝对路径，仅 24 位无压缩）

        bool operator==(const ElemCfg& o) const {
            return id == o.id && type == o.type && enabled == o.enabled &&
                   x == o.x && y == o.y && text == o.text && size == o.size &&
                   format == o.format && showWeek == o.showWeek &&
                   w == o.w && h == o.h && r == o.r && color == o.color &&
                   opacity == o.opacity && points == o.points && imagePath == o.imagePath;
        }
        bool operator!=(const ElemCfg& o) const { return !(*this == o); }
    };
    std::vector<ElemCfg> elements;                 ///< 水印元素列表（按数组顺序分配叠加区域槽位）
};

/**
 * OSD 显示参数是否发生变化（字体路径变化也算：引擎热换字体后下一拍生效）
 *
 * 仅用于非相机重启路径：没变化就跳过元素重建，避免无关配置更新
 * 导致水印闪烁；相机重启路径不看此判定，OSD 必须无条件重建。
 */
inline bool osdCfgChanged(const OsdCfg& a, const OsdCfg& b) {
    if (a.enabled != b.enabled || a.fontFile != b.fontFile) {
        return true;
    }
    /* 元素列表：数量或任一项任一字段不同都算变化（元素个数少，逐个比不心疼） */
    if (a.elements.size() != b.elements.size()) {
        return true;
    }
    for (size_t i = 0; i < a.elements.size(); ++i) {
        if (a.elements[i] != b.elements[i]) {
            return true;
        }
    }
    return false;
}

/** 拍照配置段 */
struct SnapshotCfg {
    std::string outputDir         = "/mnt/emmc/media/photos";
    int         quality           = 80;              ///< JPEG 质量（1-100）

    /** 校验 JPEG 质量 */
    bool check(std::string& errMsg) const;
};

/** 缩略图配置段 */
struct ThumbnailCfg {
    bool enabled = true;           ///< 总开关
    int  width   = 320;            ///< 缩略图宽度
    int  height  = 176;            ///< 缩略图高度
    int  quality = 60;             ///< JPEG 质量（缩略图不需要高质量）

    /** 校验尺寸范围 */
    bool check(std::string& errMsg) const;
};

/** 录像配置段 */
struct RecordCfg {
    int         segmentSec        = 300;             ///< 单段 MP4 时长（秒）
    std::string outputDir         = "/mnt/emmc/media/records";

    /** 校验分段时长（≤0 会变成永不分段，单文件无限涨到写爆磁盘） */
    bool check(std::string& errMsg) const;
};

/**
 * 触发源配置段
 *
 * 每个触发源有独立的 ID、类型、间隔、时间表等配置。
 * 支持多个触发源同时存在（如定时拍照 + 蓝牙拍照 + SOS 紧急拍照）。
 */
struct TriggerCfg {
    std::string id;                                  ///< 触发源 ID（如 "timer_snapshot"）
    std::string type;                                ///< 触发源类型（"timer" / "bluetooth" / "sos"）
    bool        enabled = false;                     ///< 开关
    int         priority = 0;                        ///< 优先级（数字越大越高：SOS=100, Motion=50, Bluetooth=30, Timer=10）
    int         intervalSec = 300;                   ///< 触发间隔（秒，仅 timer 类型）
    int         burstCount = 1;                      ///< 连拍次数
    int         burstIntervalMs = 0;                 ///< 连拍间隔（毫秒）
    bool        allDay = false;                      ///< 全天生效（忽略 schedule）
    /* 时间表 */
    std::string scheduleStart;                       ///< 起始时间（如 "10:00"，空 = 全天）
    std::string scheduleEnd;                         ///< 结束时间（如 "18:00"）
    std::vector<int> scheduleDays;                   ///< 生效的星期几（0=周日, 1=周一, ...）

    bool operator==(const TriggerCfg& o) const {
        return id == o.id && type == o.type && enabled == o.enabled &&
               priority == o.priority && intervalSec == o.intervalSec &&
               burstCount == o.burstCount && burstIntervalMs == o.burstIntervalMs &&
               allDay == o.allDay && scheduleStart == o.scheduleStart &&
               scheduleEnd == o.scheduleEnd && scheduleDays == o.scheduleDays;
    }
    bool operator!=(const TriggerCfg& o) const { return !(*this == o); }
};

/** IPC 端点配置段 */
struct IpcCfg {
    std::string iotAgentEndpoint = "ipc:///tmp/iot_agent.ipc";  ///< iot_agent 的 ROUTER 端点
};

/**
 * 实时视频配置段（P2P + SRS 降级链路）
 *
 * 设计：单人观看走 P2P 全候选 ICE（Host+Srflx+Relay 一次协商自动选路）；
 * P2P 协商超时或第 2 观看者到来 → 转 SRS（设备 WHIP 推 / 观看端 WHEP 拉）。
 * 地址全部走配置不硬编码（SRS/coturn/EMQX 沿用现有部署）。
 *
 * 本段不存设备标识：设备 ID 属 MediadConfig::deviceId（应用级），拼接地址时作为参数传入。
 */
struct LiveCfg {
    bool        enabled      = true;                 ///< 总开关（关闭则不连信令 broker）
    /* MQTT 信令配置（用于 P2P/relay 观看端协调） */
    struct MqttSignalingCfg {
        std::string url      = "tcp://localhost:1883";   ///< EMQX broker 地址
        std::string username;                                ///< MQTT 认证用户名（空=无认证）
        std::string password;                                ///< MQTT 认证密码
    } mqttSignaling;
    struct IceServerCfg {
        std::string host     = "stun.example.com";   ///< coturn（STUN/TURN 同端口，域名每次建会话时解析）
        int         port     = 3478;
        std::string username = "turn_user";             ///< TURN 长期凭证（空=仅 STUN）
        std::string password = "turn_password";
    } ice;
    /* SRS 推流地址模板：
     *   - whipUrlTemplate: 推流地址模板，包含 {deviceUid} 和 {accessToken} 占位符
     * 平台下发完整模板（含 accessToken 认证），运行时替换占位符得到最终 URL
     * 观看端拉流地址（WHEP）由后端 API 下发，设备端不关心 */
    struct SrsCfg {
        std::string whipUrlTemplate = "http://stun.example.com:1985/rtc/v1/whip/?app=live&stream={deviceUid}&accessToken={accessToken}";
    } srs;
    int p2pTimeoutSec  = 15;    ///< P2P 协商超时 → 自动转 SRS
    int relayIdleSec   = 30;    ///< Relay 模式全部心跳消失后停推窗口（观看端 10s 一跳）

    /** 设备推流地址（WHIP）：替换模板中的 {deviceUid} 和 {accessToken}
     *  @param devId 设备 ID
     *  @param accessToken 平台下发的认证令牌（可为空，模板里没有 {accessToken} 时忽略） */
    std::string whipUrl(const std::string& devId, const std::string& accessToken = "") const;
};

/** mediad 全量配置 */
struct MediadConfig {
    /**
     * 默认构造函数：初始化默认触发源配置
     * 设备启动时如果没有配置文件，会使用这个默认配置
     */
    MediadConfig();

    /**
     * 设备唯一标识（应用级，不属于任何单个服务）
     *
     * 不在本进程产生：由 IoT 业务进程从 Zigbee/provisioning 取得后经 IPC
     * CONFIG_UPDATE 下发，落盘后下次开机 mediad 自举不依赖启动顺序。
     * 信令 topic、SRS 流名、上传目录均以此为唯一来源。
     * 默认留空而非填测试值：默认值会让所有出厂设备挤在同一 topic 上互相串话。
     */
    std::string deviceId;
    CameraCfg   camera;
    OsdCfg      osd;
    SnapshotCfg snapshot;
    ThumbnailCfg thumbnail;
    RecordCfg   record;
    IpcCfg      ipc;
    LiveCfg     live;
    std::vector<TriggerCfg> triggers;                ///< 触发源配置列表

    /**
     * 全量校验（IPC 下发入口调用，启动加载不重复查）：
     * 逐个调用各段自己的 check，第一个不合规的字段写进 errMsg。
     * 只有 camera/snapshot/record 有检测函数——只拦会出大事的值
     * （编码器硬拒绝、烧 CPU、写爆磁盘）；osd/live/ipc 段靠已有
     * 容错（水印贴边修正、连不上重连），不多拦
     */
    bool check(std::string& errMsg) const;
};

class ConfigStore {
public:
    /**
     * 配置变更回调
     * @param oldCfg 变更前配置
     * @param newCfg 变更后配置
     * 监听者自行 diff 感兴趣的段；在 IPC 命令线程串行执行。
     */
    using ChangeListener = std::function<void(const MediadConfig& oldCfg,
                                              const MediadConfig& newCfg)>;

    /**
     * 启动加载配置文件
     *
     * 文件不存在/解析失败不视为致命错误：使用全默认值继续运行（兜底原则），
     * 并写出一份默认配置文件方便现场修改。
     *
     * @param path 配置文件路径
     * @return true=文件成功解析；false=使用了默认值（服务仍可运行）
     */
    bool load(const std::string& path);

    /**
     * 运行期应用局部配置更新（IPC CONFIG_UPDATE 入口）
     *
     * patch 只需携带要修改的键（如 {"snapshot":{"interval_sec":60}}），
     * 未出现的键保持当前值。合并成功后原子回写文件并通知监听者。
     *
     * @param patchJson 局部配置 JSON 文本
     * @param errMsg    失败时的原因说明
     * @return true=已生效并落盘
     */
    bool applyUpdate(const std::string& patchJson, std::string& errMsg);

    /** 获取当前配置（拷贝，线程安全） */
    MediadConfig get() const;

    /** 注册变更监听者（启动组装阶段调用，运行期不再注册） */
    void addListener(ChangeListener listener);

    /** 当前配置序列化为 JSON 文本（回写文件/状态查询共用） */
    std::string toJson() const;

private:
    /* 将 JSON 树合并到 cfg（只覆盖出现且类型正确的键，缺键保持原值） */
    static void mergeFromJson(const cJSON* root, MediadConfig& cfg);
    /* 回写当前配置到文件（原子写） */
    void persist();

    mutable std::mutex          m_mutex;
    MediadConfig                m_config;
    std::string                 m_path;
    std::vector<ChangeListener> m_listeners;
};

} // namespace mediad
