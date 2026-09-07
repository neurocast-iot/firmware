/**
 * @file http_client.cpp
 * @brief HTTP 客户端实现（上传 + 下载）
 *
 * 实现要点（经验取自已验证配方）：
 *   - 每个请求独立 easy handle，同步阻塞，天然线程安全
 *   - 必须挂空的 READFUNCTION：兜住 libcurl 的危险默认行为——当请求体来源
 *     （POSTFIELDS / MIMEPOST）因选项设置异常而缺失时，curl 会回退到
 *     fread(stdin)，进程从终端启动时 read 永久阻塞，表现为线程
 *     无日志挂死、低速检测也不生效
 *   - 低速检测（LOW_SPEED_LIMIT/TIME）替代总时长超时，防误杀大文件
 *   - 分片请求由调用方显式传总时长上限（单片大小有上界，安全）
 *   - 下载数据直接落盘（fwrite 回调），不在内存里攒
 *   - 断点续传：Range 头 + append 写入，服务器不支持则自动重头来
 */
#include "nc/http/http_client.h"
#include "curl_common.h"

#include "nc/common/log_utils.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace nc {
namespace http {

HttpClient::HttpClient(const HttpClientConfig& cfg) : m_cfg(cfg) {
    internal::ensureCurlInit();
}

HttpResponse HttpClient::perform(const std::string& url,
                                 const std::vector<std::string>& headers,
                                 long timeoutSec) {
    HttpResponse resp;

    internal::CurlHandleGuard handleGuard;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.errorMsg = "curl_easy_init failed";
        return resp;
    }
    handleGuard.handle = curl;

    /* ---- 公共选项 ---- */
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_cfg.connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, m_cfg.lowSpeedBytesPerSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_cfg.lowSpeedDurationSec);
    if (timeoutSec > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    /* ---- 自定义请求头 ---- */
    internal::CurlSlistGuard slistGuard;
    if (!headers.empty()) {
        for (const auto& h : headers) {
            slistGuard.list = curl_slist_append(slistGuard.list, h.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slistGuard.list);
    }

    /* ---- 执行 ---- */
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.errorMsg = std::string("curl error: ") + curl_easy_strerror(res);
        NC_LOGW("[http] request failed: url={} {}", url.c_str(), resp.errorMsg.c_str());
        return resp;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
    if (!resp.ok()) {
        resp.errorMsg = "HTTP " + std::to_string(resp.statusCode);
        NC_LOGW("[http] bad status: url={} code={}", url.c_str(), resp.statusCode);
    }
    return resp;
}

HttpResponse HttpClient::postFile(
    const std::string& url,
    const std::string& filePath,
    const std::vector<std::pair<std::string, std::string>>& formFields,
    const std::vector<std::string>& headers,
    long timeoutSec) {
    HttpResponse resp;

    internal::CurlHandleGuard handleGuard;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.errorMsg = "curl_easy_init failed";
        return resp;
    }
    handleGuard.handle = curl;

    /* 组装 multipart 表单：文件字段名固定 "file"（服务端约定）+ 附加字段 */
    curl_mime* mime = curl_mime_init(curl);
    if (!mime) {
        resp.errorMsg = "curl_mime_init failed";
        return resp;
    }
    /* 注意：此处不设 CURLOPT_READFUNCTION——MIMEPOST 模式下 curl 从 mime 结构读数据 */
    curl_mimepart* filePart = curl_mime_addpart(mime);
    CURLcode mimeRes = curl_mime_filedata(filePart, filePath.c_str());
    if (mimeRes != CURLE_OK) {
        resp.errorMsg = std::string("curl_mime_filedata failed: ") + curl_easy_strerror(mimeRes);
        NC_LOGW("[http] {}", resp.errorMsg.c_str());
        curl_mime_free(mime);
        return resp;
    }
    curl_mime_name(filePart, "file");
    for (const auto& kv : formFields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_data(part, kv.second.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, kv.first.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    /* 直接在本 handle 上执行，不复用 perform()（它会新建 handle 丢失 MIMEPOST） */
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_cfg.connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, m_cfg.lowSpeedBytesPerSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_cfg.lowSpeedDurationSec);
    if (timeoutSec > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    internal::CurlSlistGuard slistGuard;
    if (!headers.empty()) {
        for (const auto& h : headers) {
            slistGuard.list = curl_slist_append(slistGuard.list, h.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slistGuard.list);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.errorMsg = std::string("curl error: ") + curl_easy_strerror(res);
        NC_LOGW("[http] request failed: url={} {}", url.c_str(), resp.errorMsg.c_str());
        curl_mime_free(mime);
        return resp;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
    if (!resp.ok()) {
        resp.errorMsg = "HTTP " + std::to_string(resp.statusCode);
        NC_LOGW("[http] bad status: url={} code={}", url.c_str(), resp.statusCode);
    }

    curl_mime_free(mime);
    return resp;
}

HttpResponse HttpClient::putBinary(const std::string& url,
                                   const void* data,
                                   size_t size,
                                   const std::vector<std::string>& headers,
                                   long timeoutSec) {
    HttpResponse resp;

    internal::CurlHandleGuard handleGuard;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.errorMsg = "curl_easy_init failed";
        return resp;
    }
    handleGuard.handle = curl;

    /* PUT 二进制：CURLOPT_UPLOAD 开启上传模式，READFUNCTION 提供数据 */
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

    struct PutData {
        const uint8_t* data;
        size_t size;
        size_t offset;
    } putData{static_cast<const uint8_t*>(data), size, 0};

    auto readCallback = [](char* buffer, size_t bufSize, size_t nitems, void* userdata) -> size_t {
        auto* pd = static_cast<PutData*>(userdata);
        size_t want = bufSize * nitems;
        size_t remain = pd->size - pd->offset;
        size_t give = want < remain ? want : remain;
        if (give > 0) {
            memcpy(buffer, pd->data + pd->offset, give);
            pd->offset += give;
        }
        return give;
    };
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, +readCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &putData);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));

    /* 直接在本 handle 上执行，不复用 perform() */
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_cfg.connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, m_cfg.lowSpeedBytesPerSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_cfg.lowSpeedDurationSec);
    if (timeoutSec > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    internal::CurlSlistGuard slistGuard;
    if (!headers.empty()) {
        for (const auto& h : headers) {
            slistGuard.list = curl_slist_append(slistGuard.list, h.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slistGuard.list);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.errorMsg = std::string("curl error: ") + curl_easy_strerror(res);
        NC_LOGW("[http] request failed: url={} {}", url.c_str(), resp.errorMsg.c_str());
        return resp;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
    if (!resp.ok()) {
        resp.errorMsg = "HTTP " + std::to_string(resp.statusCode);
        NC_LOGW("[http] bad status: url={} code={}", url.c_str(), resp.statusCode);
    }
    return resp;
}

HttpResponse HttpClient::putFile(const std::string& url,
                                  const std::string& filePath,
                                  const std::vector<std::string>& headers,
                                  long timeoutSec) {
    HttpResponse resp;

    /* 先查文件大小 */
    struct stat st;
    if (::stat(filePath.c_str(), &st) != 0) {
        resp.errorMsg = "file not found: " + filePath;
        NC_LOGW("[http] {}", resp.errorMsg.c_str());
        return resp;
    }
    const size_t fileSize = static_cast<size_t>(st.st_size);

    /* 打开文件 */
    FILE* fp = std::fopen(filePath.c_str(), "rb");
    if (!fp) {
        resp.errorMsg = "failed to open file: " + filePath;
        NC_LOGW("[http] {}", resp.errorMsg.c_str());
        return resp;
    }
    internal::FileGuard fileGuard;
    fileGuard.fp = fp;

    internal::CurlHandleGuard handleGuard;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.errorMsg = "curl_easy_init failed";
        return resp;
    }
    handleGuard.handle = curl;

    /* PUT 文件：CURLOPT_UPLOAD 开启上传模式，READFUNCTION 从文件读数据 */
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

    /* fread 风格的读回调：从 FILE* 读数据 */
    auto readCallback = [](char* buffer, size_t bufSize, size_t nitems, void* userdata) -> size_t {
        FILE* f = static_cast<FILE*>(userdata);
        return std::fread(buffer, bufSize, nitems, f);
    };
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, +readCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, fp);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(fileSize));

    /* 直接在本 handle 上执行，不复用 perform() */
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_cfg.connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, m_cfg.lowSpeedBytesPerSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_cfg.lowSpeedDurationSec);
    if (timeoutSec > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    internal::CurlSlistGuard slistGuard;
    if (!headers.empty()) {
        for (const auto& h : headers) {
            slistGuard.list = curl_slist_append(slistGuard.list, h.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slistGuard.list);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.errorMsg = std::string("curl error: ") + curl_easy_strerror(res);
        NC_LOGW("[http] request failed: url={} {}", url.c_str(), resp.errorMsg.c_str());
        return resp;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
    if (!resp.ok()) {
        resp.errorMsg = "HTTP " + std::to_string(resp.statusCode);
        NC_LOGW("[http] bad status: url={} code={}", url.c_str(), resp.statusCode);
    }
    return resp;
}

HttpResponse HttpClient::postJson(const std::string& url,
                                  const std::string& jsonBody,
                                  const std::vector<std::string>& headers,
                                  long timeoutSec) {
    HttpResponse resp;

    internal::CurlHandleGuard handleGuard;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.errorMsg = "curl_easy_init failed";
        return resp;
    }
    handleGuard.handle = curl;

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, internal::readNothingCallback);

    std::vector<std::string> allHeaders = headers;
    allHeaders.push_back("Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_cfg.connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, m_cfg.lowSpeedBytesPerSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_cfg.lowSpeedDurationSec);
    if (timeoutSec > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    internal::CurlSlistGuard slistGuard;
    if (!allHeaders.empty()) {
        for (const auto& h : allHeaders) {
            slistGuard.list = curl_slist_append(slistGuard.list, h.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slistGuard.list);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.errorMsg = std::string("curl error: ") + curl_easy_strerror(res);
        NC_LOGW("[http] request failed: url={} {}", url.c_str(), resp.errorMsg.c_str());
        return resp;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
    if (!resp.ok()) {
        resp.errorMsg = "HTTP " + std::to_string(resp.statusCode);
        NC_LOGW("[http] bad status: url={} code={}", url.c_str(), resp.statusCode);
    }
    return resp;
}

HttpResponse HttpClient::del(const std::string& url,
                             const std::vector<std::string>& headers,
                             long timeoutSec) {
    HttpResponse resp;

    internal::CurlHandleGuard handleGuard;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.errorMsg = "curl_easy_init failed";
        return resp;
    }
    handleGuard.handle = curl;

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, internal::readNothingCallback);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_cfg.connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, m_cfg.lowSpeedBytesPerSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_cfg.lowSpeedDurationSec);
    if (timeoutSec > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    internal::CurlSlistGuard slistGuard;
    if (!headers.empty()) {
        for (const auto& h : headers) {
            slistGuard.list = curl_slist_append(slistGuard.list, h.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slistGuard.list);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.errorMsg = std::string("curl error: ") + curl_easy_strerror(res);
        NC_LOGW("[http] request failed: url={} {}", url.c_str(), resp.errorMsg.c_str());
        return resp;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
    if (!resp.ok()) {
        resp.errorMsg = "HTTP " + std::to_string(resp.statusCode);
        NC_LOGW("[http] bad status: url={} code={}", url.c_str(), resp.statusCode);
    }
    return resp;
}

/* ====================================================================
 * 下载侧实现
 * ==================================================================== */

namespace {

/**
 * 进度回调上下文：通过 CURLOPT_XFERINFODATA 传给 curl
 *
 * 和 HttpClient 解耦：progressBridge 是文件局部函数，不访问 HttpClient 私有成员。
 * doDownload 在栈上建一个 ProgressCtx，把指针塞进 curl 的 clientp。
 */
struct ProgressCtx {
    HttpClient::ProgressCallback cb;
    void* userdata;
};

/**
 * curl 进度回调桥接：static 函数 → ProgressCallback
 *
 * 返回非 0 让 curl 中止传输（用户回调返回 false 时取消下载）。
 */
int progressBridge(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                   curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<ProgressCtx*>(clientp);
    if (!ctx || !ctx->cb) return 0;

    int64_t total = (dltotal > 0) ? static_cast<int64_t>(dltotal) : -1;
    int64_t now   = static_cast<int64_t>(dlnow);

    return ctx->cb(now, total, ctx->userdata) ? 0 : 1;
}

} // namespace

void HttpClient::setProgressCallback(ProgressCallback cb, void* userdata) {
    m_progressCb = std::move(cb);
    m_progressUserdata = userdata;
}

/* --------------------------------------------------------------------
 * downloadToFile：基础下载（GET → 存文件，覆盖已有）
 * -------------------------------------------------------------------- */
DownloadResult HttpClient::downloadToFile(const std::string& url,
                                          const std::string& savePath,
                                          const std::vector<std::string>& headers,
                                          long timeoutSec) {
    return doDownload(url, savePath, headers, timeoutSec, 0);
}

/* --------------------------------------------------------------------
 * resumeDownload：断点续传
 *
 * 1. 本地文件不存在 → 退化为普通下载（resumeFrom=0）
 * 2. 本地文件存在 → 读已有大小，设 Range: bytes={offset}-
 * 3. 服务器返回 206 → 续传成功，追加写入
 * 4. 服务器返回 200 → 不支持续传，清掉文件重头来
 * -------------------------------------------------------------------- */
DownloadResult HttpClient::resumeDownload(const std::string& url,
                                          const std::string& savePath,
                                          const std::vector<std::string>& headers,
                                          long timeoutSec) {
    /* 查本地文件已有大小 */
    struct stat st;
    if (::stat(savePath.c_str(), &st) != 0 || st.st_size <= 0) {
        /* 文件不存在或为空，退化为普通下载 */
        return doDownload(url, savePath, headers, timeoutSec, 0);
    }

    int64_t existingSize = static_cast<int64_t>(st.st_size);
    NC_LOGI("[http] 断点续传: 已有 {} bytes, 从 offset={} 继续",
            savePath.c_str(), static_cast<long long>(existingSize));

    return doDownload(url, savePath, headers, timeoutSec, existingSize);
}

/* --------------------------------------------------------------------
 * doDownload：内部下载实现
 *
 * resumeFrom > 0 时启用断点续传逻辑（Range 头 + append 模式）。
 * 如果服务器不支持续传（返回 200 而非 206），自动清掉文件重头来。
 * -------------------------------------------------------------------- */
DownloadResult HttpClient::doDownload(const std::string& url,
                                      const std::string& savePath,
                                      const std::vector<std::string>& headers,
                                      long timeoutSec,
                                      int64_t resumeFrom) {
    DownloadResult result;

    internal::CurlHandleGuard handleGuard;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.errorMsg = "curl_easy_init failed";
        return result;
    }
    handleGuard.handle = curl;

    /* 打开文件：续传用 append，普通下载用 write（覆盖） */
    const char* openMode = (resumeFrom > 0) ? "ab" : "wb";
    FILE* fp = std::fopen(savePath.c_str(), openMode);
    if (!fp) {
        result.errorMsg = "failed to open file for writing: " + savePath;
        NC_LOGW("[http] {}", result.errorMsg.c_str());
        return result;
    }
    internal::FileGuard fileGuard;
    fileGuard.fp = fp;

    /* ---- curl 选项 ---- */
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_cfg.connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, m_cfg.lowSpeedBytesPerSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_cfg.lowSpeedDurationSec);
    if (timeoutSec > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_cfg.verifyTls ? 2L : 0L);

    /* 数据写入回调：直接落盘 */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::writeFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

    /* 进度回调 */
    ProgressCtx progressCtx;
    if (m_progressCb) {
        progressCtx.cb = m_progressCb;
        progressCtx.userdata = m_progressUserdata;
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressBridge);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressCtx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }

    /* ---- 自定义请求头 ---- */
    internal::CurlSlistGuard slistGuard;
    std::vector<std::string> allHeaders = headers;

    /* 断点续传：加 Range 头 */
    std::string rangeHeader;
    if (resumeFrom > 0) {
        rangeHeader = "Range: bytes=" + std::to_string(resumeFrom) + "-";
        allHeaders.push_back(rangeHeader);
    }

    if (!allHeaders.empty()) {
        for (const auto& h : allHeaders) {
            slistGuard.list = curl_slist_append(slistGuard.list, h.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slistGuard.list);
    }

    /* ---- 执行 ---- */
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        result.errorMsg = std::string("curl error: ") + curl_easy_strerror(res);
        NC_LOGW("[http] download failed: url={} {}", url.c_str(), result.errorMsg.c_str());
        /* 下载失败，保留已有文件（下次 resumeDownload 可继续） */
        return result;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.statusCode);

    /* ---- 处理续传响应 ---- */
    if (resumeFrom > 0 && result.statusCode == 200) {
        /* 服务器不支持 Range，返回了完整内容。
         * 但我们已经用 append 模式打开了文件，数据被追加到旧内容后面，
         * 文件内容已经损坏。需要重新从头下载。 */
        NC_LOGI("[http] 服务器不支持续传（200），清掉文件重头下载");
        fileGuard.fp = nullptr;  /* 先关文件 */
        std::fclose(fp);
        fp = nullptr;
        std::remove(savePath.c_str());

        /* 重新下载（不续传） */
        return doDownload(url, savePath, headers, timeoutSec, 0);
    }

    if (resumeFrom > 0 && result.statusCode == 206) {
        result.resumed = true;
    }

    if (!result.ok()) {
        result.errorMsg = "HTTP " + std::to_string(result.statusCode);
        NC_LOGW("[http] bad status: url={} code={}", url.c_str(), result.statusCode);
        return result;
    }

    /* 读取实际下载字节数（curl 统计） */
    double dlSize = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &dlSize);
    result.bytesDownloaded = static_cast<int64_t>(dlSize);

    NC_LOGI("[http] 下载完成: url={} saved={} bytes={} resumed={}",
            url.c_str(), savePath.c_str(),
            static_cast<long long>(result.bytesDownloaded),
            result.resumed ? "yes" : "no");

    return result;
}

} // namespace http
} // namespace nc
