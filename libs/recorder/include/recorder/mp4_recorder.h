/**
 * @file mp4_recorder.h
 * @brief MP4 录像器（平台驱动封装）
 *
 * 职责边界：只管"把送进来的编码帧写成 MP4 文件"这一件事——
 *   打开容器 → 等关键帧 → 逐帧写入 → 关键帧处按时长分段 → 收卷修复文件。
 * 不订阅相机、不起线程、不管什么时候录/录多久的业务决策，
 * 帧由调用方（如 mediad RecordService）在其回调里喂入，业务策略留在应用层。
 *
 * 配方来源：gw_av100 iot_live-cpp VideoRecorderManager 取证复刻。
 *
 * 多平台架构：
 *   - 公共层（本类）只管业务逻辑（分段、回调、文件命名）
 *   - SDK 调用全部下沉到 RecorderDriver 平台实现（如 AnykaRecorderDriver）
 *   - 新增平台 = 新写 src/<平台>/driver_factory.cpp，本类零改动
 *
 * 线程安全：所有公开方法内部加锁，可从帧回调线程与控制线程并发调用。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace recorder {

class RecorderDriver;  // 前置声明，平台驱动接口

/**
 * @brief 录像器配置
 *
 * 视频参数必须与喂入帧的实际编码参数一致（宽高/帧率/编码格式），
 * 否则容器头信息错误导致播放异常。
 */
struct RecorderConfig {
    uint16_t    width          = 1920;    ///< 视频宽度（与主通道编码一致）
    uint16_t    height         = 1080;    ///< 视频高度
    uint8_t     fps            = 25;      ///< 帧率
    bool        h265           = false;   ///< true=H.265，false=H.264
    uint32_t    segmentSeconds = 0;       ///< 分段时长（秒），0=不分段（由 stop() 收卷）
    std::string outputDir;                ///< 输出目录（内部按 yyyyMMdd/HH 建子目录）
    bool        enableAudio    = false;   ///< 音频轨（预留，当前恒为纯视频）
};

/**
 * @brief 单个文件完成回调
 *
 * 每段 MP4 收卷完成（分段切换或 stop）时触发，供上层上报/入库。
 *
 * @param filePath  完整文件路径
 * @param fileSize  文件大小（字节）
 * @param durationMs 文件时长（毫秒，修复接口返回，可能为 0）
 * @param startWallSec 这段录像第一帧写入时的墙上时间（秒），
 *                     服务器用它算文件覆盖的时间范围；没写到任何帧时为 0
 */
using FileCompleteCallback =
    std::function<void(const std::string& filePath, size_t fileSize, uint64_t durationMs,
                       uint64_t startWallSec)>;

class Mp4Recorder {
public:
    Mp4Recorder();
    ~Mp4Recorder();

    /* 句柄型资源，禁止拷贝 */
    Mp4Recorder(const Mp4Recorder&) = delete;
    Mp4Recorder& operator=(const Mp4Recorder&) = delete;

    /**
     * @brief 设置文件完成回调（须在 start 前设置）
     */
    void setFileCompleteCallback(FileCompleteCallback cb);

    /**
     * @brief 开始录制
     *
     * 打开 mux 并生成带时间戳的文件名（outputDir/yyyyMMdd/HH/yyyyMMdd_HHMMSS.mp4）。
     * 首个写入帧必须是关键帧，之前的 P 帧自动丢弃。
     *
     * @param config 录像配置
     * @return true=已进入录制状态；false=已在录制或 mux 打开失败
     */
    bool start(const RecorderConfig& config);

    /**
     * @brief 写入一帧编码数据（从帧回调线程调用）
     *
     * 内部行为（照原配方）：
     *   - 未录制/句柄无效/时间戳回退 → 丢弃
     *   - 等待关键帧阶段丢弃所有 P 帧
     *   - 关键帧处检查 segmentSeconds，到点自动收卷当前文件并开新段
     *
     * @param data       编码数据（H264/H265 NALU，含起始码）
     * @param len        数据长度
     * @param timestamp  帧时间戳（毫秒）
     * @param isKeyFrame 是否关键帧
     * @return true=写入成功；false=丢弃或写入失败
     */
    bool writeFrame(const uint8_t* data, size_t len, uint64_t timestamp, bool isKeyFrame);

    /**
     * @brief 停止录制并收卷当前文件
     *
     * 触发 FileCompleteCallback。未在录制时为空操作。
     */
    void stop();

    /**
     * @brief 是否正在录制
     */
    bool isRecording() const;

    /**
     * @brief 当前录制文件路径（未录制时为空）
     */
    std::string currentFilePath() const;

    /**
     * @brief 只重启 mux（收卷当前文件后用新参数开新 mux），不动外部订阅
     *
     * 典型场景：分段时长（segmentSeconds）或输出目录变了，只需要重建容器，
     * 不需要销毁/重建编码器和订阅。调用后录制继续，帧回调无感知。
     *
     * @param newSegmentSec 新的分段时长（秒），0=不分段
     * @return true=成功切换；false=未录制或 mux 打开失败
     */
    bool reopenMux(uint32_t newSegmentSec);

private:
    /* 内部方法均要求调用方已持有 m_mutex */
    bool openMux();                                  ///< 生成新文件名并调驱动 openAndStart
    void closeMux(bool finalizeFile);                ///< 调驱动 stopAndClose，可选收卷
    void fixAndNotify(const std::string& filePath);  ///< 调驱动 fixFile 合并临时文件并回调

    mutable std::mutex   m_mutex;
    RecorderConfig       m_config;
    FileCompleteCallback m_onFileComplete;

    std::unique_ptr<RecorderDriver> m_driver;        ///< 平台驱动实例（构造时由工厂创建）
    int      m_muxHandle         = -1;     ///< 驱动返回的 mux 句柄（-1=无效）
    bool     m_recording         = false;  ///< 录制状态
    bool     m_waitingKeyFrame   = false;  ///< 等待首个关键帧
    uint32_t m_frameSeq          = 0;      ///< 当前段内帧序号
    uint64_t m_segmentStartTs    = 0;      ///< 当前段首关键帧时间戳（分段计时基准）
    uint64_t m_lastFrameTs       = 0;      ///< 当前段最后一帧的时间戳（fix 不返回时长时用它算 duration）
    uint64_t m_segmentStartWallSec = 0;    ///< 当前段首帧写入时的墙上时间（秒，上报 start_time 用）
    std::string m_currentPath;             ///< 当前录制文件完整路径
};

} // namespace recorder
