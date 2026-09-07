/**
 * @file upload.cpp
 * @brief uploadWithPolicy 实现
 *
 * 责任划分:
 *   - 本文件管"重试 + 取消" 这 2 件事
 *   - "实际 HTTP 请求" 走 nc::http::HttpClient, 不在这里碰 libcurl
 *
 * 重试不重试的判断:
 *   - 取消 (cancel_flag)        → 不重试, 立刻返回 (用户主动停的, 重试没意义)
 *   - 文件不存在                → 不重试, 立刻返回 (文件都没有, 重试也没用)
 *   - HTTP 失败 (4xx/5xx/网络)  → 重试 (网络抖动或服务器瞬时过载, 重试可能好)
 *   - HTTP 成功 (2xx)           → 立刻返回成功
 */
#include "nc/http/upload.h"

#include "nc/common/log_utils.h"
#include "nc/http/http_client.h"

#include <chrono>
#include <sys/stat.h>
#include <thread>

namespace nc {
namespace http {

/* 内部: 单次上传尝试 (不含重试逻辑) */
namespace {

UploadWithPolicyResult doOnce(
    const std::string& url,
    const std::string& filePath,
    const UploadPolicy& policy,
    nc::http::HttpClient& http,
    const std::atomic<bool>* cancel_flag) {
    UploadWithPolicyResult r;

    /* 上传前: 检查外部取消 (用户提前按停) */
    if (cancel_flag && cancel_flag->load()) {
        r.errorMsg = "cancelled before upload";
        return r;
    }

    /* 文件检查: 不存在或为空直接失败 (不重试) */
    struct stat st;
    if (::stat(filePath.c_str(), &st) != 0 || st.st_size <= 0) {
        r.errorMsg = "file not found or empty: " + filePath;
        return r;
    }
    r.bytes = static_cast<uint64_t>(st.st_size);

    /* 实际上传: PUT 文件 */
    HttpResponse resp = http.putFile(url, filePath, policy.headers, policy.timeout_sec);

    /* 上传后: 再次检查取消 (用户在上传过程中按停) */
    if (cancel_flag && cancel_flag->load()) {
        r.errorMsg = "cancelled during upload";
        return r;
    }

    if (!resp.ok()) {
        r.errorMsg = "HTTP 失败 status=" + std::to_string(resp.statusCode) +
                     " msg=" + resp.errorMsg;
        return r;
    }

    r.ok = true;
    return r;
}

} // anonymous namespace

/**
 * 对外入口: uploadWithPolicy
 *
 * 重试循环: 每次失败按 retry_delay_sec 等待后重试, 最多 1+max_retries 次。
 * 取消/文件错误走早返回, 不进重试。
 */
UploadWithPolicyResult uploadWithPolicy(
    const std::string& url,
    const std::string& filePath,
    const UploadPolicy& policy,
    UploadProgressCallback /*progress_cb*/,
    void* /*userdata*/,
    const std::atomic<bool>* cancel_flag) {
    UploadWithPolicyResult final_result;

    /* HttpClient 在函数内构造, 每次调用独立 easy handle, 无需考虑线程安全 */
    HttpClient http(HttpClientConfig{});

    const int max_attempts = policy.max_retries + 1;  /* 1 次首发 + N 次重试 */
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        final_result = doOnce(url, filePath, policy, http, cancel_flag);
        final_result.attempts = attempt;

        if (final_result.ok) {
            NC_LOGI("[upload] 成功 attempts={} bytes={}",
                    attempt, final_result.bytes);
            return final_result;
        }

        /* 取消/文件错误: 不重试, 直接返回 (不是网络问题) */
        if (final_result.errorMsg.find("cancelled") != std::string::npos ||
            final_result.errorMsg.find("file not found") != std::string::npos) {
            NC_LOGW("[upload] 终止重试 err={}", final_result.errorMsg.c_str());
            return final_result;
        }

        if (attempt < max_attempts) {
            NC_LOGW("[upload] 第 {}/{} 次失败: {}，{}s 后重试",
                    attempt, max_attempts, final_result.errorMsg.c_str(),
                    policy.retry_delay_sec);
            /* 阻塞 sleep, 调用方要负责在合适线程调 (不能阻塞主线程) */
            std::this_thread::sleep_for(std::chrono::seconds(policy.retry_delay_sec));
        }
    }

    NC_LOGE("[upload] 失败（重试 {} 次）url={} err={}",
            policy.max_retries, url.c_str(), final_result.errorMsg.c_str());
    return final_result;
}

} // namespace http
} // namespace nc
