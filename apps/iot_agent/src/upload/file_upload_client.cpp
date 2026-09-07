/**
 * @file file_upload_client.cpp
 * @brief 文件上传客户端实现
 *
 * 协议细节（neurocast-server API 文档 §19）：
 *   - 统一响应 {"code":0,"data":...,"message":"success"}，code==0 为成功
 *   - simple upload：multipart 表单字段名 "file"，deviceUid/filename/fileHash 走 query
 *   - init 返回 uploadId / instantComplete（秒传）/ uploadedParts（断点续传进度）
 *   - 分片号从 0 开始；PUT 请求体为 application/octet-stream 原始二进制
 *   - complete 传 fileHash 供服务端合并后校验
 */
#include "upload/file_upload_client.h"

#include "nc/common/log_utils.h"
#include "nc/common/string_utils.h"
#include "nc/common/json_utils.h"

#include "cJSON.h"

#include <cstdio>
#include <cstring>

namespace iot_agent {

using nc::http::HttpResponse;

namespace {

/** 服务端接口路径（相对 baseUrl，baseUrl 已含 /api/v1/file/upload 前缀） */
constexpr const char* kPathSimple   = "/simple";
constexpr const char* kPathUploads  = "/uploads";

} // namespace

FileUploadClient::FileUploadClient(nc::http::HttpClient& http, long partTimeoutSec)
    : m_http(http), m_partTimeoutSec(partTimeoutSec) {}

bool FileUploadClient::isConfigured() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_baseUrl.empty() && !m_apiKey.empty();
}

void FileUploadClient::updateServerInfo(const std::string& baseUrl, const std::string& apiKey) {
    std::lock_guard<std::mutex> lock(m_mutex);
    /* 空串表示该字段本次不下发，保留旧值 */
    if (!baseUrl.empty()) m_baseUrl = baseUrl;
    if (!apiKey.empty()) m_apiKey = apiKey;
    NC_LOGI("[upload] server info updated: baseUrl={} apiKey={}",
            m_baseUrl.c_str(), m_apiKey.empty() ? "(empty)" : "(set)");
}

void FileUploadClient::snapshotServerInfo(std::string& baseUrl, std::string& apiKey) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    baseUrl = m_baseUrl;
    apiKey = m_apiKey;
}

std::string FileUploadClient::apiUrl(const std::string& baseUrl, const std::string& path) const {
    std::string base = baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + path;
}

std::vector<std::string> FileUploadClient::authHeaders(const std::string& apiKey) const {
    return {"X-API-KEY: " + apiKey};
}

bool FileUploadClient::parseServerResponse(const std::string& body,
                                              cJSON** dataObj,
                                              std::string& errorMsg) const {
    *dataObj = nullptr;
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        errorMsg = "服务端响应 JSON 解析失败: " + body;
        NC_LOGW("[upload] {}", errorMsg.c_str());
        return false;
    }

    const cJSON* codeItem = cJSON_GetObjectItemCaseSensitive(root, "code");
    const int code = cJSON_IsNumber(codeItem) ? codeItem->valueint : -1;
    if (code != 0) {
        const std::string msg = nc::common::JsonGetString(root, "message");
        errorMsg = msg.empty() ? "server code=" + std::to_string(code) : msg;
        NC_LOGW("[upload] 服务端业务错误: code={} message={}", code, errorMsg.c_str());
        cJSON_Delete(root);
        return false;
    }

    /* 成功：把 data 对象从 root 上摘下，所有权交给调用方，root 即可释放 */
    *dataObj = cJSON_DetachItemFromObject(root, "data");
    cJSON_Delete(root);
    return true;
}

SimpleUploadResult FileUploadClient::uploadSimple(const UploadMeta& meta,
                                                     const std::string& localPath) {
    SimpleUploadResult result;
    std::string baseUrl, apiKey;
    snapshotServerInfo(baseUrl, apiKey);
    if (baseUrl.empty() || apiKey.empty()) {
        result.errorMsg = "服务端信息未配置";
        return result;
    }

    /* 组装 query 参数：deviceUid / filename（URL 编码）/ fileHash（秒传检测） */
    std::string url = apiUrl(baseUrl, kPathSimple) + "?deviceUid=" + nc::common::UrlEncode(meta.deviceUid) +
                      "&filename=" + nc::common::UrlEncode(meta.fileName);
    if (!meta.fileHash.empty()) {
        url += "&fileHash=" + meta.fileHash;
    }

    HttpResponse resp = m_http.postFile(url, localPath, {}, authHeaders(apiKey));
    if (!resp.ok()) {
        result.errorMsg = resp.errorMsg.empty() ? "simple upload HTTP 失败" : resp.errorMsg;
        return result;
    }

    cJSON* data = nullptr;
    std::string errMsg;
    if (!parseServerResponse(resp.body, &data, errMsg)) {
        result.errorMsg = errMsg;
        return result;
    }

    if (data) {
        result.filePath = nc::common::JsonGetString(data, "filePath");
        cJSON_Delete(data);
    }
    result.ok = true;
    NC_LOGI("[upload] simple 上传成功: {} -> {}", meta.fileName.c_str(), result.filePath.c_str());
    return result;
}

InitMultipartResult FileUploadClient::initMultipart(const UploadMeta& meta) {
    InitMultipartResult result;
    std::string baseUrl, apiKey;
    snapshotServerInfo(baseUrl, apiKey);
    if (baseUrl.empty() || apiKey.empty()) {
        result.errorMsg = "服务端信息未配置";
        return result;
    }

    /* 组装 init JSON：deviceUid / filename / fileSize / fileHash / totalParts */
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "deviceUid", meta.deviceUid.c_str());
    cJSON_AddStringToObject(req, "filename", meta.fileName.c_str());
    cJSON_AddNumberToObject(req, "fileSize", static_cast<double>(meta.fileSize));
    cJSON_AddStringToObject(req, "fileHash", meta.fileHash.c_str());
    cJSON_AddNumberToObject(req, "totalParts", meta.totalParts);
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        result.errorMsg = "init 请求 JSON 组装失败";
        return result;
    }

    HttpResponse resp = m_http.postJson(apiUrl(baseUrl, kPathUploads), body, authHeaders(apiKey));
    cJSON_free(body);
    if (!resp.ok()) {
        result.errorMsg = resp.errorMsg.empty() ? "init HTTP 失败" : resp.errorMsg;
        return result;
    }

    cJSON* data = nullptr;
    std::string errMsg;
    if (!parseServerResponse(resp.body, &data, errMsg)) {
        result.errorMsg = errMsg;
        return result;
    }
    if (!data) {
        result.errorMsg = "init 响应缺少 data 字段";
        return result;
    }

    /* instantComplete=true → 秒传命中，直接完成 */
    const cJSON* instant = cJSON_GetObjectItemCaseSensitive(data, "instantComplete");
    result.instantComplete = cJSON_IsTrue(instant);

    if (result.instantComplete) {
        result.filePath = nc::common::JsonGetString(data, "filePath");
        result.ok = true;
        cJSON_Delete(data);
        NC_LOGI("[upload] 秒传命中: {} hash={}", meta.fileName.c_str(), meta.fileHash.c_str());
        return result;
    }

    result.uploadId = nc::common::JsonGetString(data, "uploadId");
    if (result.uploadId.empty()) {
        result.errorMsg = "init 返回 uploadId 为空";
        cJSON_Delete(data);
        return result;
    }

    /* 断点续传：解析服务端已存在的分片号 */
    const cJSON* parts = cJSON_GetObjectItemCaseSensitive(data, "uploadedParts");
    if (cJSON_IsArray(parts)) {
        for (int i = 0; i < cJSON_GetArraySize(parts); ++i) {
            const cJSON* item = cJSON_GetArrayItem(parts, i);
            if (cJSON_IsNumber(item)) {
                result.uploadedParts.push_back(item->valueint);
            }
        }
    }
    cJSON_Delete(data);

    result.ok = true;
    NC_LOGI("[upload] init 成功: uploadId={} totalParts={} 已传分片={} file={}",
            result.uploadId.c_str(), meta.totalParts, result.uploadedParts.size(),
            meta.fileName.c_str());
    return result;
}

bool FileUploadClient::uploadPart(const std::string& uploadId,
                                     int partNumber,
                                     const uint8_t* data,
                                     size_t size,
                                     std::string& errorMsg) {
    std::string baseUrl, apiKey;
    snapshotServerInfo(baseUrl, apiKey);
    if (baseUrl.empty() || apiKey.empty()) {
        errorMsg = "服务端信息未配置";
        return false;
    }

    std::string url = apiUrl(baseUrl, std::string("/uploads/") + uploadId + "/parts") +
                      "?partNumber=" + std::to_string(partNumber);
    /* 单片总时长兜底：分片大小有上界，此处用总时长超时不会误杀大文件 */
    HttpResponse resp =
        m_http.putBinary(url, data, size, authHeaders(apiKey), m_partTimeoutSec);
    if (!resp.ok()) {
        errorMsg = resp.errorMsg.empty() ? "part HTTP 失败" : resp.errorMsg;
        return false;
    }

    /* 分片响应只校验业务码，无 data 内容 */
    cJSON* respData = nullptr;
    const bool ok = parseServerResponse(resp.body, &respData, errorMsg);
    if (respData) cJSON_Delete(respData);
    return ok;
}

CompleteMultipartResult FileUploadClient::completeMultipart(const std::string& uploadId,
                                                               const std::string& fileHash) {
    CompleteMultipartResult result;
    std::string baseUrl, apiKey;
    snapshotServerInfo(baseUrl, apiKey);
    if (baseUrl.empty() || apiKey.empty()) {
        result.errorMsg = "服务端信息未配置";
        return result;
    }

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "fileHash", fileHash.c_str());
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        result.errorMsg = "complete 请求 JSON 组装失败";
        return result;
    }

    HttpResponse resp = m_http.postJson(
        apiUrl(baseUrl, std::string("/uploads/") + uploadId + "/complete"), body, authHeaders(apiKey));
    cJSON_free(body);
    if (!resp.ok()) {
        result.errorMsg = resp.errorMsg.empty() ? "complete HTTP 失败" : resp.errorMsg;
        return result;
    }

    cJSON* data = nullptr;
    std::string errMsg;
    if (!parseServerResponse(resp.body, &data, errMsg)) {
        result.errorMsg = errMsg;
        return result;
    }

    if (data) {
        result.filePath = nc::common::JsonGetString(data, "filePath");
        cJSON_Delete(data);
    }
    result.ok = true;
    NC_LOGI("[upload] complete 成功: uploadId={} -> {}", uploadId.c_str(), result.filePath.c_str());
    return result;
}

bool FileUploadClient::abortMultipart(const std::string& uploadId) {
    std::string baseUrl, apiKey;
    snapshotServerInfo(baseUrl, apiKey);
    if (baseUrl.empty() || apiKey.empty()) {
        return false;
    }

    std::string url = apiUrl(baseUrl, "/uploads/" + uploadId);
    HttpResponse resp = m_http.del(url, authHeaders(apiKey));
    if (!resp.ok()) {
        NC_LOGW("[upload] abort 请求失败: uploadId={} {}", uploadId.c_str(), resp.errorMsg.c_str());
        return false;
    }

    cJSON* data = nullptr;
    std::string errMsg;
    const bool ok = parseServerResponse(resp.body, &data, errMsg);
    if (data) cJSON_Delete(data);
    return ok;
}

} // namespace iot_agent
