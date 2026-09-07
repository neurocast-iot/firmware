/**
 * @file http_client.h
 * @brief HTTP 客户端 —— 基于 libcurl 的同步请求工具（上传 + 下载）
 *
 * 职责：
 *   - 上传侧：multipart 上传 / PUT 二进制 / POST JSON / DELETE
 *   - 下载侧：文件下载（流式写入） + 断点续传 + 进度回调
 *   - 统一超时策略（连接超时 + 低速卡死检测 + 可选总时长兜底）
 *   - 统一错误处理（curl 错误码 / HTTP 状态码）
 *
 * 设计说明：
 *   - 每次调用独立创建/释放 easy handle，天然线程安全，多线程并发调用无问题
 *   - 不设置全局总时长超时（CURLOPT_TIMEOUT）：大文件传输耗时长，总时长超时会
 *     误杀大文件；改用 CURLOPT_LOW_SPEED_* 低速检测，只在真正无进展时中止
 *   - 嵌入式环境无 CA 证书，TLS 证书校验默认关闭
 *   - 下载数据直接落盘（fwrite 回调），不在内存里攒（嵌入式内存有限）
 *   - 断点续传：本地文件存在时自动检测大小，发 Range 头；
 *     服务器不支持（返回 200 而非 206）则清掉重头来
 *
 * 使用方式：
 *   nc::http::HttpClient client(nc::http::HttpClientConfig{});
 *
 *   // 上传
 *   auto resp = client.postJson(url, "{}", {"X-API-KEY: xxx"});
 *
 *   // 下载
 *   auto result = client.downloadToFile("https://example.com/file.bin", "/tmp/file.bin");
 *
 *   // 断点续传
 *   auto result = client.resumeDownload("https://example.com/file.bin", "/tmp/file.bin");
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nc {
namespace http {

/**
 * HTTP 客户端配置（上传/下载共用）
 *
 * 默认值取自已验证配方，嵌入式弱网环境下经过实测。
 */
struct HttpClientConfig {
    long connectTimeoutSec = 60;       /* 建连超时（TCP/TLS），不限制数据传输时长 */
    long lowSpeedBytesPerSec = 10240;  /* 低速阈值：速率低于 10KB/s 视为卡死 */
    long lowSpeedDurationSec = 30;     /* 持续低速超过 30 秒则中止请求 */
    bool verifyTls = false;            /* TLS 证书校验：嵌入式环境无 CA，默认关闭 */
};

/** HTTP 响应结果（上传侧 / 通用请求用） */
struct HttpResponse {
    long statusCode = 0;      /* HTTP 状态码；0 表示网络层失败（curl 错误） */
    std::string body;         /* 响应体 */
    std::string errorMsg;     /* 失败时的错误描述 */

    /** 请求是否成功（2xx） */
    bool ok() const { return statusCode >= 200 && statusCode < 300; }
};

/** 下载结果 */
struct DownloadResult {
    long statusCode = 0;          /* HTTP 状态码；0 表示网络层失败 */
    int64_t bytesDownloaded = 0;  /* 本次下载的字节数（续传时不含已有部分） */
    bool resumed = false;         /* 是否是续传（206 Partial Content） */
    std::string errorMsg;         /* 失败时的错误描述 */

    /** 下载是否成功（2xx） */
    bool ok() const { return statusCode >= 200 && statusCode < 300; }
};

/**
 * @brief HTTP 客户端（上传 + 下载统一入口）
 *
 * 一个实例同时支持上传和下载操作，所有方法线程安全。
 */
class HttpClient {
public:
    explicit HttpClient(const HttpClientConfig& cfg);

    /* ======== 上传侧 ======== */

    /**
     * @brief POST multipart/form-data 上传本地文件
     *
     * 文件以表单字段名 "file" 提交，另可附加普通表单字段。
     *
     * @param url        完整请求 URL（含 query 参数）
     * @param filePath   本地文件路径
     * @param formFields 附加表单字段（key, value），可为空
     * @param headers    自定义请求头（如 "X-API-KEY: xxx"），可为空
     * @param timeoutSec 请求总时长上限（秒），-1=不限（用低速检测），>0 显式限制
     */
    HttpResponse postFile(const std::string& url,
                          const std::string& filePath,
                          const std::vector<std::pair<std::string, std::string>>& formFields = {},
                          const std::vector<std::string>& headers = {},
                          long timeoutSec = -1);

    /**
     * @brief PUT 二进制数据（application/octet-stream）
     */
    HttpResponse putBinary(const std::string& url,
                           const void* data,
                           size_t size,
                           const std::vector<std::string>& headers = {},
                           long timeoutSec = -1);

    /**
     * @brief PUT 本地文件（application/octet-stream，流式读取，不整体载入内存）
     *
     * @param url        完整请求 URL
     * @param filePath   本地文件路径
     * @param headers    自定义请求头（如 "X-API-KEY: xxx"），可为空
     * @param timeoutSec 请求总时长上限（秒），-1=不限（用低速检测），>0 显式限制
     */
    HttpResponse putFile(const std::string& url,
                         const std::string& filePath,
                         const std::vector<std::string>& headers = {},
                         long timeoutSec = -1);

    /**
     * @brief POST JSON 请求体（application/json）
     */
    HttpResponse postJson(const std::string& url,
                          const std::string& jsonBody,
                          const std::vector<std::string>& headers = {},
                          long timeoutSec = -1);

    /**
     * @brief DELETE 请求（无请求体）
     */
    HttpResponse del(const std::string& url,
                     const std::vector<std::string>& headers = {},
                     long timeoutSec = -1);

    /* ======== 下载侧 ======== */

    /**
     * @brief 基础下载：GET URL → 存本地文件
     *
     * 文件不存在则创建，已存在则覆盖。数据流式写入（fwrite 回调）。
     *
     * @param url        下载 URL
     * @param savePath   本地保存路径
     * @param headers    自定义请求头（如 "Authorization: Bearer xxx"），可为空
     * @param timeoutSec 请求总时长上限（秒），-1=不限（用低速检测），>0 显式限制
     */
    DownloadResult downloadToFile(const std::string& url,
                                  const std::string& savePath,
                                  const std::vector<std::string>& headers = {},
                                  long timeoutSec = -1);

    /**
     * @brief 断点续传：从本地已有位置继续下载
     *
     * 逻辑：
     *   1. 本地文件不存在 → 退化为普通下载
     *   2. 本地文件存在 → 读已有大小，设 Range: bytes={offset}-
     *   3. 服务器返回 206 → 续传成功，追加写入
     *   4. 服务器返回 200 → 不支持续传，清掉文件重头来
     *   5. 下载失败 → 保留已下载部分（下次继续）
     *
     * @param url        下载 URL
     * @param savePath   本地保存路径（已有文件会追加）
     * @param headers    自定义请求头，可为空
     * @param timeoutSec 请求总时长上限（秒），-1=不限
     */
    DownloadResult resumeDownload(const std::string& url,
                                  const std::string& savePath,
                                  const std::vector<std::string>& headers = {},
                                  long timeoutSec = -1);

    /**
     * @brief 设置下载进度回调
     *
     * 回调参数：(已下载字节, 总字节, 用户数据)
     * 总字节为 -1 表示服务器未返回 Content-Length（未知总量）。
     * 回调返回 false 则取消下载。
     */
    using ProgressCallback = std::function<bool(int64_t downloaded, int64_t total, void* userdata)>;
    void setProgressCallback(ProgressCallback cb, void* userdata = nullptr);

private:
    /**
     * @brief 组装公共选项并执行请求（上传侧通用路径）
     */
    HttpResponse perform(const std::string& url,
                         const std::vector<std::string>& headers,
                         long timeoutSec);

    /**
     * @brief 内部下载实现
     *
     * @param resumeFrom 续传起始偏移（0=不续传，从头下载）
     */
    DownloadResult doDownload(const std::string& url,
                              const std::string& savePath,
                              const std::vector<std::string>& headers,
                              long timeoutSec,
                              int64_t resumeFrom);

    HttpClientConfig m_cfg;

    /* 进度回调（下载用） */
    ProgressCallback m_progressCb;
    void* m_progressUserdata = nullptr;
};

} // namespace http
} // namespace nc
