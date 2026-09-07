/**
 * @file file_upload_client.h
 * @brief 文件上传客户端 —— 对接 neurocast-server 设备文件上传接口（API 文档 §19）
 *
 * 实现 IUploadClient 接口，把 S3/OSS 风格的分片语义映射到服务端 5 个接口：
 *   - POST /api/device/file/upload/simple                        小文件直传
 *   - POST /api/device/file/upload/uploads                       初始化分片
 *   - PUT  /api/device/file/upload/uploads/{id}/parts?partNumber=N  上传分片
 *   - POST /api/device/file/upload/uploads/{id}/complete         完成合并
 *   - DELETE /api/device/file/upload/uploads/{id}                中止上传
 *
 * 认证：X-API-KEY 请求头 + deviceUid 参数（凭证来自云端共享属性下发，
 * 由 updateServerInfo 动态更新）
 */

/**
 * File upload client — interfaces with neurocast-server device file upload API (API doc §19)
 *
 * Implements IUploadClient, mapping S3/OSS-style multipart semantics to 5 server endpoints:
 *   - POST /api/device/file/upload/simple                        small file direct upload
 *   - POST /api/device/file/upload/uploads                       init multipart
 *   - PUT  /api/device/file/upload/uploads/{id}/parts?partNumber=N  upload part
 *   - POST /api/device/file/upload/uploads/{id}/complete         complete merge
 *   - DELETE /api/device/file/upload/uploads/{id}                abort upload
 *
 * Auth: X-API-KEY header + deviceUid param (credentials from cloud shared attributes,
 * dynamically updated by updateServerInfo)
 */
#pragma once

#include "nc/http/http_client.h"
#include "upload/upload_client.h"
#include "cJSON.h"

#include <mutex>
#include <string>
#include <vector>

namespace iot_agent {

class FileUploadClient : public IUploadClient {
public:
    /**
     * @param http           共享 HTTP 客户端（同步接口，worker 线程并发调用安全）
     * @param partTimeoutSec 单个分片请求的总时长上限（秒）：单片大小有硬上界，
     *                       用总时长超时不会误杀大文件，是低速检测之外的最后兜底
     */
    explicit FileUploadClient(nc::http::HttpClient& http, long partTimeoutSec = 120);

    bool isConfigured() const override;
    void updateServerInfo(const std::string& baseUrl, const std::string& apiKey) override;

    SimpleUploadResult uploadSimple(const UploadMeta& meta, const std::string& localPath) override;
    InitMultipartResult initMultipart(const UploadMeta& meta) override;
    bool uploadPart(const std::string& uploadId,
                    int partNumber,
                    const uint8_t* data,
                    size_t size,
                    std::string& errorMsg) override;
    CompleteMultipartResult completeMultipart(const std::string& uploadId,
                                              const std::string& fileHash) override;
    bool abortMultipart(const std::string& uploadId) override;

private:
    /** 读取当前服务端信息快照（baseUrl/apiKey 由云端线程更新，需加锁） */
    void snapshotServerInfo(std::string& baseUrl, std::string& apiKey) const;

    /** 拼接 API 路径（去掉 baseUrl 尾部斜杠 + 追加接口路径） */
    std::string apiUrl(const std::string& baseUrl, const std::string& path) const;

    /** 组装 X-API-KEY 请求头列表 */
    std::vector<std::string> authHeaders(const std::string& apiKey) const;

    /**
     * 解析服务端统一响应 {"code":0,"data":...,"message":"..."}
     * @return true=业务成功（code==0），成功时 dataObj 指向 data 对象（调用方持有 root）
     */
    bool parseServerResponse(const std::string& body,
                             cJSON** dataObj,
                             std::string& errorMsg) const;

    nc::http::HttpClient& m_http;
    long m_partTimeoutSec;  /* 单分片请求总时长上限（秒） */

    mutable std::mutex m_mutex;
    std::string m_baseUrl;  /* 服务端地址（http://host:port） */
    std::string m_apiKey;   /* X-API-KEY 鉴权值 */
};

} // namespace iot_agent
