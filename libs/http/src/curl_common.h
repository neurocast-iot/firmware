/**
 * @file curl_common.h
 * @brief libcurl 公共工具（内部头文件，不对外暴露）
 *
 * HttpClient 上传和下载共用的 curl 基础设施：
 *   - 全局初始化（进程级一次，多线程安全）
 *   - 响应写入回调（追加到 std::string / 写入 FILE*）
 *   - 空读回调（兜住 curl 危险默认行为，详见 http_client.cpp 文件头注释）
 *   - RAII guard（curl handle / slist 自动释放）
 */
#pragma once

#include <curl/curl.h>

#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

namespace nc {
namespace http {
namespace internal {

/** curl 全局初始化（进程级一次，多线程调用安全） */
inline void ensureCurlInit() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

/** curl 响应写入回调：追加到 std::string */
inline size_t writeStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* str = static_cast<std::string*>(userdata);
    str->append(ptr, size * nmemb);
    return size * nmemb;
}

/**
 * curl 响应写入回调：追加写入 FILE*
 *
 * 下载用：数据直接落盘，不在内存里攒（嵌入式内存有限，大文件不能全存内存）。
 * 返回写入字节数，和 curl 期望一致；写入失败返回 0 让 curl 中止传输。
 */
inline size_t writeFileCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* fp = static_cast<FILE*>(userdata);
    size_t written = std::fwrite(ptr, size, nmemb, fp);
    return written;
}

/**
 * curl 请求体读取回调：永远返回 0（EOF）
 *
 * 兜底防挂死：请求体来源缺失时 curl 会退回默认 READFUNCTION（fread(stdin)），
 * 挂上本回调后同样的问题退化为 0 字节 body 并快速失败
 */
inline size_t readNothingCallback(char* /*buffer*/, size_t /*size*/, size_t /*nitems*/, void* /*userdata*/) {
    return 0;
}

/** curl handle 的 RAII 包装：作用域结束自动清理 */
struct CurlHandleGuard {
    CURL* handle = nullptr;
    ~CurlHandleGuard() {
        if (handle) curl_easy_cleanup(handle);
    }
};

/** curl_slist 的 RAII 包装：作用域结束自动释放 */
struct CurlSlistGuard {
    curl_slist* list = nullptr;
    ~CurlSlistGuard() {
        if (list) curl_slist_free_all(list);
    }
};

/** FILE* 的 RAII 包装：作用域结束自动关闭 */
struct FileGuard {
    FILE* fp = nullptr;
    ~FileGuard() {
        if (fp) std::fclose(fp);
    }
};

} // namespace internal
} // namespace http
} // namespace nc
