/**
 * @file upload.h
 * @brief 高层上传：HttpClient + 重试 + 取消 (自由函数, libs/http/ 一员)
 *
 * 与 HttpClient 的关系:
 *   - HttpClient        底层: 1 次 HTTP PUT, 返回 HttpResponse
 *   - uploadWithPolicy  高层: N 次 HTTP PUT, 返回 UploadWithPolicyResult
 *
 * 为什么这一层存在:
 *   - 单纯调 HttpClient::uploadFile, 拿到失败结果还得自己写重试 for 循环
 *   - 这些是任何"上传文件"场景的通用需求, 跟具体业务没关系, 但跟 HTTP 有关系
 *   - 放在 libs/http/ 里, 跟 HttpClient 协作, 不单独成库
 *
 * 用法:
 *   #include "nc/http/upload.h"
 *   nc::http::UploadPolicy policy;
 *   policy.max_retries = 3;
 *   auto r = nc::http::uploadWithPolicy(
 *       "https://example.com/upload", "/mnt/emmc/data/file.mp4", policy);
 *   if (!r.ok) { NC_LOGE("上传失败: {}", r.errorMsg.c_str()); }
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nc {
namespace http {

/**
 * 进度回调: (已上传字节, 总字节, 用户数据); 返回 false 取消上传
 *
 * 与 HttpClient::ProgressCallback 签名一致
 */
using UploadProgressCallback =
    std::function<bool(int64_t uploaded, int64_t total, void* userdata)>;

/**
 * 高层上传策略参数
 *
 * 决定 HttpClient 失败时怎么办（重试几次、间隔多久）。
 * HttpClient 本身的策略 (连接超时 / 低速检测) 在 HttpClientConfig 里, 不要混到这里。
 */
struct UploadPolicy {
    /* 重试策略: 单次上传失败后重试几次。max_retries=3 表示总共最多 1+3=4 次尝试 */
    int max_retries = 3;

    /* 重试间隔 (秒): 失败后等多久再试一次。固定间隔 */
    int retry_delay_sec = 5;

    /* 单次请求总时长上限 (秒), 传给 HttpClient 内部用。
     * -1 表示不限, 走 HttpClient 的低速检测逻辑 */
    int timeout_sec = 600;

    /* 自定义请求头（如 "X-API-KEY: xxx"），可为空 */
    std::vector<std::string> headers;
};

/**
 * 高层上传结果 (带重试统计)
 *
 * ok=false 时看 errorMsg; ok=true 时看 bytes / attempts。
 */
struct UploadWithPolicyResult {
    bool ok = false;                /* 是否成功 */
    uint64_t bytes = 0;             /* 上传的字节数 */
    int attempts = 0;               /* 实际尝试次数 (1 表示首战告捷, N 表示重试了 N-1 次) */
    std::string errorMsg;           /* 失败时的错误信息 (成功时为空) */
};

/**
 * @brief 高层上传入口: HttpClient + UploadPolicy 组合
 *
 * 行为:
 *   1) 用 HttpClient::uploadFile 上传文件 (PUT 方式)
 *   2) 按 policy.max_retries 重试; 失败间隔 policy.retry_delay_sec 秒
 *   3) 每次重试前检查 cancel_flag, 置 true 立即终止
 *   4) 取消错误不重试 (这不是网络问题, 重试也没用)
 *   5) 每次失败原因透传到 UploadWithPolicyResult::errorMsg
 *
 * 线程: 阻塞调用, 内部 sleep + HttpClient 同步 IO。
 *       调用方负责在合适线程调, 阻塞主线程会卡 UI / 心跳。
 *
 * @param url          上传 URL (HTTP/HTTPS)
 * @param filePath     本地文件路径
 * @param policy       策略参数 (见 UploadPolicy)
 * @param progress_cb  可选进度回调; nullptr=不回调
 * @param userdata     透传给 progress_cb 的用户数据
 * @param cancel_flag  可选取消开关, atomic<bool>*; nullptr=不响应外部取消
 * @return             结果结构
 */
UploadWithPolicyResult uploadWithPolicy(
    const std::string& url,
    const std::string& filePath,
    const UploadPolicy& policy,
    UploadProgressCallback progress_cb = nullptr,
    void* userdata = nullptr,
    const std::atomic<bool>* cancel_flag = nullptr);

} // namespace http
} // namespace nc
