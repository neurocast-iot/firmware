/**
 * @file download.cpp
 * @brief downloadWithPolicy + checkDiskSpace 实现
 *
 * 责任划分:
 *   - 本文件管"重试 + 磁盘预检 + 取消" 这 3 件事
 *   - "实际 HTTP 请求" 走 nc::http::HttpClient, 不在这里碰 libcurl
 *
 * 重试不重试的判断:
 *   - 取消 (cancel_flag)        → 不重试, 立刻返回 (用户主动停的, 重试没意义)
 *   - 磁盘空间不足              → 不重试, 立刻返回 (磁盘不够装, 重试也装不下)
 *   - HTTP 失败 (4xx/5xx/网络)  → 重试 (网络抖动或服务器瞬时过载, 重试可能好)
 *   - HTTP 成功 (2xx)           → 立刻返回成功
 */
#include "nc/http/download.h"

#include "nc/common/log_utils.h"
#include "nc/http/http_client.h"

#include <chrono>
#include <sys/statvfs.h>
#include <thread>

namespace nc {
namespace http {

/**
 * 磁盘空间检查 (statvfs 包装)
 *
 * @note 嵌入式文件系统 (jffs2 / ubifs) 部分实现可能不支持 statvfs, 调用失败时
 *       按"不够用"处理 (return false), 让上层走"没做预检"的路径继续下载
 */
bool DownloadPolicy::checkDiskSpace(const std::string& path, uint64_t required) {
    struct statvfs vfs;
    if (::statvfs(path.c_str(), &vfs) != 0) {
        NC_LOGW("[download] statvfs 失败 path={}", path.c_str());
        return false;
    }
    /* f_bavail: 非特权用户可用的块数; f_frsize: 每块大小 (字节) */
    const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    if (free_bytes < required) {
        NC_LOGW("[download] 磁盘空间不足 free={}MB need={}MB",
                free_bytes / (1024 * 1024), required / (1024 * 1024));
        return false;
    }
    return true;
}

/* 内部: 单次下载尝试 (不含重试逻辑) */
namespace {

DownloadWithPolicyResult doOnce(
    const std::string& url,
    const std::string& savePath,
    const DownloadPolicy& policy,
    nc::http::HttpClient& http,
    const std::atomic<bool>* cancel_flag) {
    DownloadWithPolicyResult r;

    /* 下载前: 检查外部取消 (用户提前按停) */
    if (cancel_flag && cancel_flag->load()) {
        r.errorMsg = "cancelled before download";
        return r;
    }

    /* 磁盘空间预检: 拿 max_file_size 当 "最大可能需要的空间" 查一次。
     * 不查实际文件大小 (服务器 Content-Length 还没拿), 保守按上限查 */
    if (policy.disk_check) {
        if (!DownloadPolicy::checkDiskSpace(savePath, policy.max_file_size)) {
            r.errorMsg = "磁盘空间不足 (need " +
                         std::to_string(policy.max_file_size) + " bytes)";
            return r;
        }
    }

    /* 实际下载: 续传开关开 → resumeDownload, 关 → 普通 downloadToFile
     * HttpClient 内部处理 Range 头, 服务器不支持时自动降级到 200 + 清文件重头 */
    DownloadResult dl;
    if (policy.resume_support) {
        dl = http.resumeDownload(url, savePath, {}, policy.timeout_sec);
        r.resumed = dl.resumed;
    } else {
        dl = http.downloadToFile(url, savePath, {}, policy.timeout_sec);
    }

    /* 下载后: 再次检查取消 (用户在下载过程中按停) */
    if (cancel_flag && cancel_flag->load()) {
        r.errorMsg = "cancelled during download";
        return r;
    }

    if (!dl.ok()) {
        r.errorMsg = "HTTP 失败 status=" + std::to_string(dl.statusCode) +
                     " msg=" + dl.errorMsg;
        return r;
    }

    r.ok = true;
    r.bytes = static_cast<uint64_t>(dl.bytesDownloaded);
    return r;
}

} // anonymous namespace

/**
 * 对外入口: downloadWithPolicy
 *
 * 重试循环: 每次失败按 retry_delay_sec 等待后重试, 最多 1+max_retries 次。
 * 取消/磁盘错误走早返回, 不进重试。
 */
DownloadWithPolicyResult downloadWithPolicy(
    const std::string& url,
    const std::string& savePath,
    const DownloadPolicy& policy,
    DownloadProgressCallback progress_cb,
    void* userdata,
    const std::atomic<bool>* cancel_flag) {
    DownloadWithPolicyResult final_result;

    /* HttpClient 在函数内构造, 每次调用独立 easy handle, 无需考虑线程安全 */
    HttpClient http(HttpClientConfig{});
    if (progress_cb) {
        /* lambda 捕获 progress_cb 和 userdata, HttpClient 自己的 userdata 字段
         * 传了但不读 (lambda 忽略 void* ud), 不算浪费: 避免误以为没传 */
        http.setProgressCallback(
            [progress_cb](int64_t downloaded, int64_t total, void* /*ud*/) -> bool {
                return progress_cb(downloaded, total, nullptr);
            },
            userdata);
    }

    const int max_attempts = policy.max_retries + 1;  /* 1 次首发 + N 次重试 */
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        final_result = doOnce(url, savePath, policy, http, cancel_flag);
        final_result.attempts = attempt;

        if (final_result.ok) {
            NC_LOGI("[download] 成功 attempts={} bytes={} resumed={}",
                    attempt, final_result.bytes,
                    final_result.resumed ? "yes" : "no");
            return final_result;
        }

        /* 取消/磁盘错误: 不重试, 直接返回 (不是网络问题) */
        if (final_result.errorMsg.find("cancelled") != std::string::npos ||
            final_result.errorMsg.find("磁盘空间不足") != std::string::npos) {
            NC_LOGW("[download] 终止重试 err={}", final_result.errorMsg.c_str());
            return final_result;
        }

        if (attempt < max_attempts) {
            NC_LOGW("[download] 第 {}/{} 次失败: {}，{}s 后重试",
                    attempt, max_attempts, final_result.errorMsg.c_str(),
                    policy.retry_delay_sec);
            /* 阻塞 sleep, 调用方要负责在合适线程调 (不能阻塞主线程) */
            std::this_thread::sleep_for(std::chrono::seconds(policy.retry_delay_sec));
        }
    }

    NC_LOGE("[download] 失败（重试 {} 次）url={} err={}",
            policy.max_retries, url.c_str(), final_result.errorMsg.c_str());
    return final_result;
}

} // namespace http
} // namespace nc
