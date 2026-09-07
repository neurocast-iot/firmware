#include "nc/common/crypto_utils.h"

#include "nc/common/log_utils.h"

#include <fstream>
#include <iomanip>
#include <sstream>

extern "C" {
#include <openssl/sha.h>
#include <openssl/evp.h>
}

namespace nc {
namespace common {

// 计算文件 SHA256，用 8KB 分块喂 OpenSSL，避免大文件整体载入内存
// 失败返回空字符串（打开文件失败、读取异常）
std::string CalculateFileSHA256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        NC_LOGE("[SHA256] 打开文件失败 path={}", file_path.c_str());
        return "";
    }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    constexpr size_t kBufSize = 8192;
    char buf[kBufSize];
    while (file.read(buf, kBufSize)) {
        SHA256_Update(&ctx, buf, file.gcount());
    }
    if (file.gcount() > 0) {
        SHA256_Update(&ctx, buf, file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// 计算文件 MD5，用 EVP 流式计算，8KB 分块读取
// 失败返回空字符串（打开文件失败、读取异常）
std::string CalculateFileMD5(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        NC_LOGE("[MD5] 打开文件失败 path={}", file_path.c_str());
        return "";
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1) {
        if (ctx) EVP_MD_CTX_free(ctx);
        NC_LOGE("[MD5] 上下文初始化失败");
        return "";
    }

    constexpr size_t kBufSize = 8192;
    char buf[kBufSize];
    while (file.read(buf, kBufSize)) {
        EVP_DigestUpdate(ctx, buf, file.gcount());
    }
    if (file.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, file.gcount());
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    EVP_DigestFinal_ex(ctx, digest, &digestLen);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i) {
        oss << std::setw(2) << static_cast<int>(digest[i]);
    }
    return oss.str();
}

} // namespace common
} // namespace nc
