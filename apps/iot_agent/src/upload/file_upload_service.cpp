/**
 * @file file_upload_service.cpp
 * @brief 文件上传服务实现
 *
 * 并发模型：
 *   - 单队列多消费者：worker 用「谓词版 wait + 持锁 pop」消费，天然线程安全
 *   - 谓词包含两个条件：队列非空 且 存储凭据就绪；凭据未下发时任务排队等待，
 *     不占用 worker 也不消耗重试次数
 *
 * 重试策略：
 *   - 单任务最多尝试 maxRetries 次（含首次），失败退避 2s/4s/8s 指数递增
 *   - multipart 失败时先 abort 清理服务端临时分片再重试，重试重新 init，
 *     服务端返回 uploadedParts 实现断点续传（已传分片不重传）
 */
#include "upload/file_upload_service.h"

#include "nc/common/log_utils.h"
#include "nc/common/crypto_utils.h"

#include "cJSON.h"

#include <sys/types.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace iot_agent {

namespace {

constexpr int kWorkerCount = 2;  /* 见头文件"关键设计"：视频不堵图片，4G 上行限吞吐 */

} // namespace

FileUploadService::FileUploadService(IUploadClient& uploadClient) : m_uploadClient(uploadClient) {}

FileUploadService::~FileUploadService() {
    stop();
}

void FileUploadService::setDeviceUid(const std::string& uid) {
    m_deviceUid = uid;
}

void FileUploadService::start(const UploadServiceConfig& cfg) {
    if (m_running.load()) {
        NC_LOGW("[upload] 服务已在运行，忽略重复 start");
        return;
    }
    m_config = cfg;
    m_running.store(true);
    for (int i = 0; i < kWorkerCount; ++i) {
        m_workers.emplace_back(&FileUploadService::workerLoop, this, i);
    }
    NC_LOGI("[upload] 服务已启动: workers={} 队列上限={} 重试={} 分片={}KB",
            kWorkerCount, m_config.queueCapacity, m_config.maxRetries,
            static_cast<long>(m_config.partSizeBytes / 1024));
}

void FileUploadService::stop() {
    const bool wasRunning = m_running.exchange(false);
    if (!wasRunning) return;

    /* notify_all 而非 notify_one：唤醒全部 worker 才能让它们观察到 m_running=false */
    m_cv.notify_all();
    for (auto& t : m_workers) {
        if (t.joinable()) t.join();
    }
    m_workers.clear();

    /* 清空队列：内存队列不做断电持久化，重启后不补传历史文件（简单稳定） */
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }
    NC_LOGI("[upload] 服务已停止");
}

bool FileUploadService::enqueue(const UploadTask& task) {
    if (task.localPath.empty() || task.fileName.empty()) {
        NC_LOGW("[upload] 入队失败：路径或文件名为空");
        return false;
    }
    if (!m_running.load()) {
        NC_LOGW("[upload] 入队失败：服务未运行 (file={})", task.fileName.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        /* 队列满丢最旧：内存有限，宁可丢最早的文件也不无限积压 */
        if (static_cast<int>(m_queue.size()) >= m_config.queueCapacity) {
            const UploadTask& dropped = m_queue.front();
            NC_LOGW("[upload] 队列已满({})，丢弃最旧任务: {}",
                    m_config.queueCapacity, dropped.fileName.c_str());
            m_queue.pop_front();
        }
        m_queue.push_back(task);
    }
    m_cv.notify_one();

    NC_LOGI("[upload] 入队成功: file={} type={} size={} 排队={}",
            task.fileName.c_str(), task.fileType.c_str(),
            static_cast<long long>(task.fileSize), pendingCount());
    return true;
}

void FileUploadService::updateServerInfo(const std::string& baseUrl, const std::string& apiKey) {
    m_uploadClient.updateServerInfo(baseUrl, apiKey);
    /* 凭据可能从无到有，唤醒等待中的 worker 开始消化队列 */
    m_cv.notify_all();
}

void FileUploadService::setResultCallback(std::function<void(const UploadOutcome&)> cb) {
    std::lock_guard<std::mutex> lock(m_cbMutex);
    m_resultCallback = std::move(cb);
}

int FileUploadService::pendingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}

void FileUploadService::workerLoop(int workerId) {
    NC_LOGI("[upload][w{}] worker 启动", workerId);

    while (m_running.load()) {
        UploadTask task;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this]() {
                return !m_running.load() || (!m_queue.empty() && m_uploadClient.isConfigured());
            });
            if (!m_running.load()) break;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }

        UploadOutcome out;
        out.fileName = task.fileName;
        out.fileType = task.fileType;
        out.localPath = task.localPath;
        out.fileSize = task.fileSize;
        out.fileId = task.fileId;  /* 透传云端下发的文件 ID，结果遥测原样回传 */

        processTask(task, out);

        NC_LOGI("[upload][w{}] 任务结束: file={} ok={} 尝试={}",
                workerId, task.fileName.c_str(), out.ok ? 1 : 0, out.attempts);

        /* 触发结果回调（worker 线程内，回调方勿做耗时操作） */
        std::function<void(const UploadOutcome&)> cb;
        {
            std::lock_guard<std::mutex> lock(m_cbMutex);
            cb = m_resultCallback;
        }
        if (cb) cb(out);
    }

    NC_LOGI("[upload][w{}] worker 退出", workerId);
}

void FileUploadService::processTask(const UploadTask& task, UploadOutcome& out) {
    /* 先算 MD5：文件不存在/为空在此拦截（不重试）；hash 供秒传与合并校验 */
    std::string fileHash = nc::common::CalculateFileMD5(task.localPath);
    if (fileHash.empty()) {
        out.errorMessage = "MD5 计算失败（文件不存在或无法打开）: " + task.localPath;
        NC_LOGW("[upload] MD5 计算失败: file={}", task.fileName.c_str());
        return;
    }

    /* 重试上限：首次尝试 + maxRetries 次重试（默认共 4 次，退避 2s/4s/8s） */
    for (int attempt = 1; attempt <= m_config.maxRetries + 1; ++attempt) {
        if (!m_running.load()) return;  /* 服务停止，放弃任务 */

        if (attempt > 1) {
            NC_LOGW("[upload] 重试 {}/{}: file={} 上次错误={}",
                    attempt, m_config.maxRetries, task.fileName.c_str(), out.errorMessage.c_str());
            if (waitBackoff(attempt)) return;  /* 退避期间被 stop 打断 */
        }

        const bool ok = (task.fileType == "video")
                            ? uploadViaMultipart(task, fileHash, out)
                            : uploadViaSimple(task, fileHash, out);
        out.attempts = attempt;
        if (ok) {
            out.ok = true;
            return;
        }
    }

    /* 重试耗尽：丢弃任务，错误信息经回调上报云端 */
    NC_LOGE("[upload] 任务失败（重试耗尽）: file={} {}", task.fileName.c_str(),
            out.errorMessage.c_str());
}

bool FileUploadService::uploadViaSimple(const UploadTask& task,
                                        const std::string& fileHash,
                                        UploadOutcome& out) {
    UploadMeta meta;
    meta.deviceUid = m_deviceUid;
    meta.fileName = task.fileName;
    meta.fileSize = task.fileSize;
    meta.fileHash = fileHash;

    const SimpleUploadResult r = m_uploadClient.uploadSimple(meta, task.localPath);
    out.remotePath = r.filePath;
    if (!r.ok) {
        out.errorMessage = r.errorMsg;
    }
    return r.ok;
}

bool FileUploadService::uploadViaMultipart(const UploadTask& task,
                                           const std::string& fileHash,
                                           UploadOutcome& out) {
    /* 分片数 = ceil(fileSize / partSize)，最少 1 片 */
    const int64_t partSize = m_config.partSizeBytes;
    const int64_t totalParts = (task.fileSize + partSize - 1) / partSize;
    if (totalParts <= 0) {
        out.errorMessage = "文件为空";
        return false;
    }

    /* 1. init：秒传检测 + 断点续传进度恢复 */
    UploadMeta meta;
    meta.deviceUid = m_deviceUid;
    meta.fileName = task.fileName;
    meta.fileSize = task.fileSize;
    meta.fileHash = fileHash;
    meta.totalParts = static_cast<int>(totalParts);

    const InitMultipartResult init = m_uploadClient.initMultipart(meta);
    if (!init.ok) {
        out.errorMessage = init.errorMsg;
        return false;
    }
    if (init.instantComplete) {
        out.remotePath = init.filePath;
        out.instantComplete = true;
        NC_LOGI("[upload] 秒传完成: file={}", task.fileName.c_str());
        return true;
    }

    /* 2. 逐片上传，跳过服务端已存在的分片 */
    FILE* fp = std::fopen(task.localPath.c_str(), "rb");
    if (!fp) {
        out.errorMessage = "无法打开文件: " + task.localPath;
        m_uploadClient.abortMultipart(init.uploadId);
        return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(partSize));
    bool failed = false;

    for (int64_t i = 0; i < totalParts && m_running.load(); ++i) {
        /* 断点续传：init 返回的 uploadedParts 直接跳过 */
        if (std::find(init.uploadedParts.begin(), init.uploadedParts.end(),
                      static_cast<int>(i)) != init.uploadedParts.end()) {
            continue;
        }

        const int64_t offset = i * partSize;
        const int64_t remain = task.fileSize - offset;
        const size_t readSize = static_cast<size_t>(remain < partSize ? remain : partSize);

        /* fseeko 用 64 位偏移：32 位平台的 fseek(long) 在 2GB 处溢出 */
        if (fseeko(fp, static_cast<off_t>(offset), SEEK_SET) != 0) {
            out.errorMessage = "fseek 失败: offset=" + std::to_string(offset);
            failed = true;
            break;
        }
        if (std::fread(buf.data(), 1, readSize, fp) != readSize) {
            out.errorMessage = "读取分片数据不完整: part=" + std::to_string(i);
            failed = true;
            break;
        }

        std::string partErr;
        if (!m_uploadClient.uploadPart(init.uploadId, static_cast<int>(i), buf.data(), readSize,
                                  partErr)) {
            out.errorMessage = partErr.empty() ? "分片上传失败" : partErr;
            failed = true;
            break;
        }
        NC_LOGI("[upload] 分片 {}/{} 完成: {} 字节 file={}",
                static_cast<long long>(i + 1), static_cast<long long>(totalParts),
                static_cast<long long>(readSize), task.fileName.c_str());
    }
    std::fclose(fp);

    if (failed || !m_running.load()) {
        /* 失败/停止时 abort 清理服务端临时分片（也可依赖服务端 TTL，双保险） */
        m_uploadClient.abortMultipart(init.uploadId);
        return false;
    }

    /* 3. complete：服务端合并分片并校验 MD5 */
    const CompleteMultipartResult done = m_uploadClient.completeMultipart(init.uploadId, fileHash);
    if (!done.ok) {
        out.errorMessage = done.errorMsg;
        /* complete 失败说明数据不完整（如分片损坏），abort 后重试全流程 */
        m_uploadClient.abortMultipart(init.uploadId);
        return false;
    }

    out.remotePath = done.filePath;
    NC_LOGI("[upload] 分片上传完成: file={} 分片数={}", task.fileName.c_str(),
            static_cast<long long>(totalParts));
    return true;
}

bool FileUploadService::waitBackoff(int attempt) {
    /* 指数退避：2s / 4s / 8s ...（attempt 从 2 开始，位移计算） */
    const int delaySec = m_config.retryBaseDelaySec << (attempt - 2);
    NC_LOGI("[upload] 退避等待 {}s 后重试", delaySec);

    std::unique_lock<std::mutex> lk(m_mutex);
    /* wait_for 支持 stop 打断：停止时立即返回 true，worker 放弃任务退出 */
    return m_cv.wait_for(lk, std::chrono::seconds(delaySec),
                         [this]() { return !m_running.load(); });
}

} // namespace iot_agent
