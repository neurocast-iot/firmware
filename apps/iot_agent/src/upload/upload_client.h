/**
 * @file upload_client.h
 * @brief 上传客户端抽象接口 —— 按 S3/OSS multipart 语义定义的上传协议
 *
 * 职责边界：
 *   - 上层（FileUploadService）只关心"把一个文件送到远端"，不关心协议细节
 *   - 本接口按业界事实标准（S3 MultipartUpload / OSS InitiateMultipartUpload）
 *     建模：init → part × N → complete / abort，语义与五步流程一一对应
 *   - 未来扩展 OSS/S3 直传时，只需新增实现类（OssUploadClient /
 *     S3UploadClient），认证方式不同但流程语义一致，上层零改动
 *
 * 断点续传约定：
 *   - init 返回 uploadedParts（服务端已存在的分片号），上层跳过这些分片
 *   - init 返回 instantComplete=true 表示秒传命中（MD5 相同），无需传分片
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iot_agent {

/** 上传文件元信息（init/simple 共用） */
struct UploadMeta {
    std::string deviceUid;  /* 设备唯一标识 */
    std::string fileName;   /* 远端文件名（不含目录） */
    int64_t fileSize = 0;   /* 文件总大小（字节） */
    std::string fileHash;   /* 文件 MD5（秒传检测 + 合并校验） */
    int totalParts = 0;     /* 分片总数（multipart 用） */
};

/** simple upload 结果 */
struct SimpleUploadResult {
    bool ok = false;
    std::string filePath;    /* 服务端文件路径 */
    std::string errorMsg;
};

/** init multipart 结果 */
struct InitMultipartResult {
    bool ok = false;
    bool instantComplete = false;    /* true=秒传命中，无需上传分片 */
    std::string uploadId;            /* 分片上传任务 ID */
    std::vector<int> uploadedParts;  /* 断点续传：服务端已有分片号 */
    std::string filePath;            /* 秒传时返回的服务端路径 */
    std::string errorMsg;
};

/** complete multipart 结果 */
struct CompleteMultipartResult {
    bool ok = false;
    std::string filePath;   /* 合并后的服务端文件路径 */
    std::string errorMsg;
};

/**
 * @brief 上传客户端抽象接口
 *
 * 实现类（FileUploadClient / 未来的 OssUploadClient）通过构造函数
 * 注入 nc::http::HttpClient，认证信息（baseUrl/apiKey）由 updateServerInfo 动态下发。
 */
class IUploadClient {
public:
    virtual ~IUploadClient() = default;

    /**
     * @brief 认证信息是否就绪
     *
     * 未就绪时 FileUploadService 的 worker 不取任务（任务排队等待），
     * 云端下发 baseUrl/apiKey 后恢复处理。
     */
    virtual bool isConfigured() const = 0;

    /**
     * @brief 动态更新服务端地址与 API Key（云端属性下发时调用）
     *
     * @param baseUrl 服务端地址（如 http://host:port），空串表示不更新
     * @param apiKey  X-API-KEY 鉴权值，空串表示不更新
     */
    virtual void updateServerInfo(const std::string& baseUrl, const std::string& apiKey) = 0;

    /** 小文件直传（multipart/form-data 单次请求） */
    virtual SimpleUploadResult uploadSimple(const UploadMeta& meta, const std::string& localPath) = 0;

    /** 初始化分片上传（含秒传检测与断点续传进度恢复） */
    virtual InitMultipartResult initMultipart(const UploadMeta& meta) = 0;

    /**
     * 上传单个分片（PUT 二进制）
     * @param partNumber 分片号（从 0 开始）
     * @return true=该分片已成功落服务端
     */
    virtual bool uploadPart(const std::string& uploadId,
                            int partNumber,
                            const uint8_t* data,
                            size_t size,
                            std::string& errorMsg) = 0;

    /** 完成分片上传（服务端合并 + MD5 校验） */
    virtual CompleteMultipartResult completeMultipart(const std::string& uploadId,
                                                      const std::string& fileHash) = 0;

    /** 中止分片上传（清理服务端临时分片） */
    virtual bool abortMultipart(const std::string& uploadId) = 0;
};

} // namespace iot_agent
