/**
 * @file file_upload_service.h
 * @brief 文件上传服务 —— 单队列 + 多 worker + 重试编排
 *
 * 职责：
 *   - 接收 IpcEventHandler 解析出的上传任务（原图/缩略图/视频），非阻塞入队
 *   - worker 线程从队列取任务，按文件类型分派：
 *       image / thumbnail → simple 直传
 *       video             → multipart 分片（2MB/片，断点续传 + 秒传）
 *   - 失败重试（指数退避 2s/4s/8s），耗尽丢弃并回调上报
 *
 * 关键设计（经验取自 gw_av100 的 FileUploadService）：
 *   - 2 个 worker：视频可能传数分钟，单线程会造成队头阻塞，让图片跟着等；
 *     4G 上行带宽固定，超过 2 个只会互相摊薄速率，不增加总吞吐
 *   - 凭据未下发（baseUrl/apiKey 为空）时 worker 不取任务，任务排队等待，
 *     云端属性下发后 notify 唤醒恢复（"上传服务待命，任务排队，收到后恢复"）
 *   - 队列上限 100，超出丢最旧：内存有限，宁可丢老文件也不无限积压
 *   - 断点续传靠服务端 uploadedParts：重试时重新 init，跳过已传分片
 */
#pragma once

#include "upload/upload_client.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iot_agent {

/** 上传任务（由 IpcEventHandler 从 MEDIA_FILE_READY 事件解析，或由 RPC uploadFile 指令下发） */
struct UploadTask {
    std::string localPath;  /* 本地文件完整路径 */
    std::string fileName;   /* 文件名（远端保存同名） */
    std::string fileType;   /* "image" / "thumbnail" / "video" */
    int64_t fileSize = 0;   /* 文件大小（字节） */
    std::string fileId;     /* 文件唯一 ID（RPC 按需上传时由云端下发，结果遥测原样回传；可为空） */
};

/** 上传结果（每个任务结束时通过回调上报） */
struct UploadOutcome {
    bool ok = false;
    std::string fileName;
    std::string fileType;
    std::string localPath;
    std::string remotePath;       /* 服务端文件路径（成功时） */
    int64_t fileSize = 0;
    int attempts = 0;             /* 实际尝试次数 */
    bool instantComplete = false; /* 秒传命中 */
    std::string errorMessage;
    std::string fileId;           /* 透传任务的 fileId，云端用来关联是哪个文件的上传结果 */
};

/** 上传服务行为配置 */
struct UploadServiceConfig {
    int maxRetries = 3;                      /* 失败后重试次数（总尝试 = maxRetries+1），退避 2s/4s/8s */
    int retryBaseDelaySec = 2;               /* 重试退避基数（2s/4s/8s 指数递增） */
    int queueCapacity = 100;                 /* 队列上限，超出丢最旧 */
    int64_t partSizeBytes = 2 * 1024 * 1024; /* 分片大小（2MB，经验值） */
};

class FileUploadService {
public:
    /** @param uploadClient 上传客户端实现（FileUploadClient / 未来 Oss/S3） */
    explicit FileUploadService(IUploadClient& uploadClient);
    ~FileUploadService();

    /* 禁止拷贝（持线程与队列） */
    FileUploadService(const FileUploadService&) = delete;
    FileUploadService& operator=(const FileUploadService&) = delete;

    /** 设置设备唯一标识（deviceUid，上传请求必带） */
    void setDeviceUid(const std::string& uid);

    /** 启动 worker 线程（幂等） */
    void start(const UploadServiceConfig& cfg);

    /** 停止服务：唤醒全部 worker 退出并清空队列 */
    void stop();

    /**
     * 入队上传任务（非阻塞）
     * @return true=入队成功；false=服务未运行或任务非法
     */
    bool enqueue(const UploadTask& task);

    /** 云端属性下发时更新服务端地址/API Key，并唤醒等待中的 worker */
    void updateServerInfo(const std::string& baseUrl, const std::string& apiKey);

    /** 设置结果回调（worker 线程触发，勿做耗时操作） */
    void setResultCallback(std::function<void(const UploadOutcome&)> cb);

    /** 当前排队任务数 */
    int pendingCount() const;

private:
    /** worker 主循环：取任务（队列非空 + 凭据就绪）→ 处理 → 回调 */
    void workerLoop(int workerId);

    /** 处理单个任务：重试编排 + 分派 simple/multipart */
    void processTask(const UploadTask& task, UploadOutcome& out);

    /** 小文件直传（image/thumbnail） */
    bool uploadViaSimple(const UploadTask& task, const std::string& fileHash, UploadOutcome& out);

    /** 大文件分片上传（video）：init → part × N → complete */
    bool uploadViaMultipart(const UploadTask& task, const std::string& fileHash, UploadOutcome& out);

    /**
     * 重试前退避等待
     * @return true=等待期间被 stop 打断（放弃任务）
     */
    bool waitBackoff(int attempt);

    IUploadClient& m_uploadClient;

    /* deviceUid 由 main 启动时设置，worker 只读（设置必在 start 之前） */
    std::string m_deviceUid;

    UploadServiceConfig m_config; /* start 之后只读 */

    std::atomic<bool> m_running{false};
    std::vector<std::thread> m_workers;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<UploadTask> m_queue;

    std::mutex m_cbMutex;
    std::function<void(const UploadOutcome&)> m_resultCallback;
};

} // namespace iot_agent
