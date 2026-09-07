/**
 * @file download.h
 * @brief 高层下载：HttpClient + 重试 + 磁盘预检 + 取消 (自由函数, libs/http/ 一员)
 *
 * 与 HttpClient 的关系:
 *   - HttpClient        底层: 1 次 HTTP GET, 返回 DownloadResult
 *   - downloadWithPolicy 高层: N 次 HTTP GET, 返回 DownloadWithPolicyResult
 *
 * 为什么这一层存在:
 *   - 单纯调 HttpClient::downloadToFile, 拿到失败结果还得自己写重试 for 循环
 *   - 单纯调 HttpClient::downloadToFile 不知道磁盘够不够, 下到 99% 才崩才傻
 *   - 这些是任何"下载大文件"场景的通用需求, 跟 OTA 没关系, 但跟 HTTP 有关系
 *   - 放在 libs/http/ 里, 跟 HttpClient 协作, 不单独成库
 *
 * 用法:
 *   #include "nc/http/download.h"
 *   nc::http::DownloadPolicy policy;
 *   policy.max_retries = 3;
 *   auto r = nc::http::downloadWithPolicy(
 *       "https://example.com/pkg.bin", "/mnt/emmc/ota/inbox/pkg.bin", policy);
 *   if (!r.ok) { NC_LOGE("下载失败: {}", r.errorMsg.c_str()); }
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace nc {
namespace http {

/**
 * 进度回调: (已下载字节, 总字节, 用户数据); 返回 false 取消下载
 *
 * 与 HttpClient::ProgressCallback 签名一致, 放在命名空间层级方便自由函数使用。
 * 不要跟 HttpClient::ProgressCallback 混了, 它是类内嵌套类型, 这个是命名空间级。
 */
using DownloadProgressCallback =
    std::function<bool(int64_t downloaded, int64_t total, void* userdata)>;

/**
 * 高层下载策略参数
 *
 * 这是"会自我保护的下载器"的策略, 决定 HttpClient 失败时怎么办。
 * HttpClient 本身的策略 (连接超时 / 低速检测) 在 HttpClientConfig 里, 不要混到这里。
 */
struct DownloadPolicy {
    /* 重试策略: 单次下载失败后重试几次。max_retries=3 表示总共最多 1+3=4 次尝试 */
    int max_retries = 3;

    /* 重试间隔 (秒): 失败后等多久再试一次。固定间隔, 指数退避留 TODO */
    int retry_delay_sec = 5;

    /* 单次请求总时长上限 (秒), 传给 HttpClient 内部用。
     * -1 表示不限, 走 HttpClient 的低速检测逻辑 */
    int timeout_sec = 600;

    /* 文件大小上限 (字节): 超过这个值的文件直接拒绝下载 (不下到一半才发现) */
    uint64_t max_file_size = 256ULL * 1024 * 1024;  /* 默认 256MB */

    /* 断点续传开关: 服务器支持时自动 Range, 服务器不支持时自动从头下载 */
    bool resume_support = true;

    /* 磁盘空间预检: 下载前先 statvfs 看剩余空间够不够装 max_file_size */
    bool disk_check = true;

    /**
     * @brief 检查 path 所在文件系统剩余空间是否够装 required 字节
     *
     * 用 statvfs 拿 f_bavail * f_frsize (非特权用户可用空间, 不是总空间)。
     *
     * @param path     任意路径 (一般是下载目标文件所在目录的某个文件)
     * @param required 需要的字节数
     * @return true=够用; false=不够或 statvfs 调用失败
     */
    static bool checkDiskSpace(const std::string& path, uint64_t required);
};

/**
 * 高层下载结果 (带重试统计)
 *
 * 与 HttpClient::DownloadResult 的区别: 多一个 attempts 字段 (实际尝试次数)。
 * ok=false 时看 errorMsg; ok=true 时看 bytes / resumed / attempts。
 */
struct DownloadWithPolicyResult {
    bool ok = false;                /* 是否成功 */
    bool resumed = false;           /* 是否走的断点续传 (服务器返回 206) */
    uint64_t bytes = 0;             /* 本次下载的字节数 (续传时不含已有部分) */
    int attempts = 0;               /* 实际尝试次数 (1 表示首战告捷, N 表示重试了 N-1 次) */
    std::string errorMsg;           /* 失败时的错误信息 (成功时为空) */
};

/**
 * @brief 高层下载入口: HttpClient + DownloadPolicy 组合
 *
 * 行为:
 *   1) 启动时先看 policy.disk_check 决定要不要 statvfs
 *   2) 按 policy.max_retries 重试; 失败间隔 policy.retry_delay_sec 秒
 *   3) 每次重试前检查 cancel_flag, 置 true 立即终止
 *   4) 取消/磁盘错误不重试 (这些不是网络问题, 重试也没用)
 *   5) 每次失败原因透传到 DownloadWithPolicyResult::errorMsg
 *
 * 线程: 阻塞调用, 内部 sleep + HttpClient 同步 IO。
 *       调用方负责在合适线程调, 阻塞主线程会卡 UI / 心跳。
 *
 * @param url          下载 URL (HTTP/HTTPS)
 * @param savePath     本地保存路径 (会被覆盖或追加, 取决于 resume_support)
 * @param policy       策略参数 (见 DownloadPolicy)
 * @param progress_cb  可选进度回调; nullptr=不回调
 * @param userdata     透传给 progress_cb 的用户数据
 *                     (HttpClient 自己的 userdata 字段未用, 走 lambda 捕获)
 * @param cancel_flag  可选取消开关, atomic<bool>*; nullptr=不响应外部取消
 * @return             结果结构
 */
DownloadWithPolicyResult downloadWithPolicy(
    const std::string& url,
    const std::string& savePath,
    const DownloadPolicy& policy,
    DownloadProgressCallback progress_cb = nullptr,
    void* userdata = nullptr,
    const std::atomic<bool>* cancel_flag = nullptr);

} // namespace http
} // namespace nc
