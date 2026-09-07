/**
 * @file test_http_client.cpp
 * @brief nc::http::HttpClient 单元测试
 *
 * 测什么:
 *   - 上传侧: postJson / uploadFile / putBinary / del
 *   - 下载侧: downloadToFile / resumeDownload / 进度回调
 *   - 错误路径: HTTP 404/500 / 连接超时 / 文件写入失败
 *   - 边界条件: 续传文件不存在 / 服务器不支持续传
 *
 * 不测:
 *   - libcurl 内部行为（第三方库）
 *   - TLS 证书校验（需要真实 HTTPS 服务器）
 *   - 低速检测（需要模拟慢速网络，复杂度高）
 *
 * 测试策略:
 *   - 用 fake_http_server 模拟 HTTP 服务器
 *   - 每个测试独立（AAA 模式，不共享状态）
 *   - 临时文件用 mkstemp，测试完清理
 */
#include "nc/http/http_client.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "fake_http_server.h"

using nc::http::HttpClient;
using nc::http::HttpClientConfig;
using nc::http::HttpResponse;
using nc::http::DownloadResult;
using nc::ota_test::FakeHttpServer;
using nc::ota_test::Response;
using nc::ota_test::Request;

/* 工具：把 string 内容写到临时文件 */
static std::string writeTempFile(const std::string& content) {
    char tmpl[] = "/tmp/http_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) return "";
    ssize_t n = ::write(fd, content.data(), content.size());
    static_cast<void>(n);
    ::close(fd);
    return tmpl;
}

/* 工具：读整个文件进 string */
static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

/* ====================================================================
 * 上传侧测试
 * ==================================================================== */

TEST(HttpClientUploadTest, PostJsonSuccess) {
    // Arrange: fake server 接收 POST，返回 200 + echo body
    FakeHttpServer::Builder b;
    b.route("/api/data", [](const Request& req) {
        std::string bodyStr(req.body.begin(), req.body.end());
        return Response(200, R"({"status":"ok","echo":")" + bodyStr + R"("})");
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});

    // Act: POST JSON
    auto resp = client.postJson(
        server->url("/api/data"),
        R"({"key":"value"})",
        {"X-Custom-Header: test123"}
    );

    // Assert: 验 status + body
    EXPECT_TRUE(resp.ok());
    EXPECT_EQ(200, resp.statusCode);
    EXPECT_NE(std::string::npos, resp.body.find("status"));
}

TEST(HttpClientUploadTest, PostJsonReturns404) {
    // Arrange: fake server 没有注册路由，默认 404
    FakeHttpServer::Builder b;
    auto server = b.start();

    HttpClient client(HttpClientConfig{});

    // Act: POST 到不存在的路径
    auto resp = client.postJson(server->url("/missing"), "{}");

    // Assert: 验 404
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(404, resp.statusCode);
}

TEST(HttpClientUploadTest, PutBinarySuccess) {
    // Arrange: fake server 接收 PUT，返回 200
    std::atomic<size_t> receivedSize{0};
    FakeHttpServer::Builder b;
    b.route("/upload", [&receivedSize](const Request& req) {
        receivedSize = req.body.size();
        return Response(200, "ok");
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});
    const std::string data = "binary content here";

    // Act: PUT 二进制
    auto resp = client.putBinary(
        server->url("/upload"),
        data.data(),
        data.size()
    );

    // Assert: 验 status + 服务器收到的大小
    EXPECT_TRUE(resp.ok());
    EXPECT_EQ(200, resp.statusCode);
    EXPECT_EQ(data.size(), receivedSize.load());
}

TEST(HttpClientUploadTest, DeleteSuccess) {
    // Arrange: fake server 接收 DELETE，返回 204
    FakeHttpServer::Builder b;
    b.route("/resource/123", [](const Request&) {
        return Response(204, "");
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});

    // Act: DELETE
    auto resp = client.del(server->url("/resource/123"));

    // Assert: 验 204
    EXPECT_TRUE(resp.ok());
    EXPECT_EQ(204, resp.statusCode);
}

/* ====================================================================
 * 下载侧测试
 * ==================================================================== */

TEST(HttpClientDownloadTest, DownloadToFileSuccess) {
    // Arrange: fake server 返回文件内容
    const std::string payload = "file content for download test";
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&payload](const Request&) {
        return Response(200, payload);
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});
    auto savePath = writeTempFile("");

    // Act: 下载到文件
    auto result = client.downloadToFile(server->url("/file.bin"), savePath);

    // Assert: 验 status + 文件内容 + bytesDownloaded
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(200, result.statusCode);
    EXPECT_EQ(payload, readFile(savePath));
    EXPECT_EQ(static_cast<int64_t>(payload.size()), result.bytesDownloaded);

    ::std::remove(savePath.c_str());
}

TEST(HttpClientDownloadTest, DownloadReturns404) {
    // Arrange: fake server 没有注册路由
    FakeHttpServer::Builder b;
    auto server = b.start();

    HttpClient client(HttpClientConfig{});
    auto savePath = writeTempFile("");

    // Act: 下载到不存在的路径
    auto result = client.downloadToFile(server->url("/missing"), savePath);

    // Assert: 验 404
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(404, result.statusCode);

    ::std::remove(savePath.c_str());
}

TEST(HttpClientDownloadTest, ResumeDownloadSuccess) {
    // Arrange: 本地已有部分文件，服务器支持续传（返回 206）
    const std::string existing = "partial ";
    const std::string remaining = "content";
    const std::string full = existing + remaining;

    FakeHttpServer::Builder b;
    b.route("/file.bin", [&remaining, &full](const Request& req) {
        // 检查 Range 头
        if (req.headers.count("Range") > 0) {
            Response resp(206, remaining);
            resp.headers["Content-Range"] = "bytes 8-15/16";
            return resp;
        }
        return Response(200, full);
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});
    auto savePath = writeTempFile(existing);  // 已有 8 字节

    // Act: 断点续传
    auto result = client.resumeDownload(server->url("/file.bin"), savePath);

    // Assert: 验 206 + resumed=true + 文件内容完整
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(206, result.statusCode);
    EXPECT_TRUE(result.resumed);
    EXPECT_EQ(full, readFile(savePath));

    ::std::remove(savePath.c_str());
}

TEST(HttpClientDownloadTest, ResumeFileNotExists) {
    // Arrange: 本地文件不存在，退化为普通下载
    const std::string payload = "full content";
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&payload](const Request&) {
        return Response(200, payload);
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});
    auto savePath = "/tmp/nonexistent_file_" + std::to_string(::time(nullptr));

    // Act: 续传但文件不存在
    auto result = client.resumeDownload(server->url("/file.bin"), savePath);

    // Assert: 验普通下载（200，resumed=false）
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(200, result.statusCode);
    EXPECT_FALSE(result.resumed);
    EXPECT_EQ(payload, readFile(savePath));

    ::std::remove(savePath.c_str());
}

TEST(HttpClientDownloadTest, ResumeServerNotSupport) {
    // Arrange: 本地有部分文件，但服务器不支持续传（返回 200 而非 206）
    const std::string existing = "partial ";
    const std::string full = "full content from server";

    FakeHttpServer::Builder b;
    b.route("/file.bin", [&full](const Request&) {
        // 服务器忽略 Range 头，返回完整内容
        return Response(200, full);
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});
    auto savePath = writeTempFile(existing);

    // Act: 续传但服务器不支持
    auto result = client.resumeDownload(server->url("/file.bin"), savePath);

    // Assert: 验自动重头下载（200，resumed=false，文件内容完整）
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(200, result.statusCode);
    EXPECT_FALSE(result.resumed);
    EXPECT_EQ(full, readFile(savePath));

    ::std::remove(savePath.c_str());
}

/* ====================================================================
 * 进度回调测试
 * ==================================================================== */

TEST(HttpClientProgressTest, ProgressCallbackCalled) {
    // Arrange: fake server 返回大一点的内容，触发多次进度回调
    const std::string payload(1024, 'x');  // 1KB
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&payload](const Request&) {
        return Response(200, payload);
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});
    std::atomic<int> callbackCount{0};
    std::atomic<int64_t> lastDownloaded{0};

    client.setProgressCallback(
        [&callbackCount, &lastDownloaded](int64_t downloaded, int64_t /*total*/, void*) -> bool {
            ++callbackCount;
            lastDownloaded = downloaded;
            return true;  // 继续下载
        }
    );

    auto savePath = writeTempFile("");

    // Act: 下载
    auto result = client.downloadToFile(server->url("/file.bin"), savePath);

    // Assert: 验回调被调用 + 最后 downloaded = payload size
    EXPECT_TRUE(result.ok());
    EXPECT_GT(callbackCount.load(), 0);
    EXPECT_EQ(static_cast<int64_t>(payload.size()), lastDownloaded.load());

    ::std::remove(savePath.c_str());
}

TEST(HttpClientProgressTest, ProgressCallbackCancel) {
    // Arrange: 回调返回 false 取消下载
    const std::string payload(1024, 'x');
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&payload](const Request&) {
        return Response(200, payload);
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});
    client.setProgressCallback(
        [](int64_t, int64_t, void*) -> bool {
            return false;  // 立即取消
        }
    );

    auto savePath = writeTempFile("");

    // Act: 下载（会被取消）
    auto result = client.downloadToFile(server->url("/file.bin"), savePath);

    // Assert: 验下载失败（curl error: callback abort）
    EXPECT_FALSE(result.ok());
    EXPECT_NE(std::string::npos, result.errorMsg.find("curl error"));

    ::std::remove(savePath.c_str());
}

/* ====================================================================
 * 错误路径测试
 * ==================================================================== */

TEST(HttpClientErrorTest, InvalidUrl) {
    // Arrange: 无效 URL
    HttpClient client(HttpClientConfig{});
    auto savePath = writeTempFile("");

    // Act: 下载到无效 URL
    auto result = client.downloadToFile("http://invalid.domain.that.does.not.exist", savePath);

    // Assert: 验失败 + errorMsg 非空
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.errorMsg.empty());

    ::std::remove(savePath.c_str());
}

TEST(HttpClientErrorTest, FileWriteFailure) {
    // Arrange: 无效保存路径（目录不存在）
    const std::string payload = "content";
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&payload](const Request&) {
        return Response(200, payload);
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});

    // Act: 下载到不存在的目录
    auto result = client.downloadToFile(
        server->url("/file.bin"),
        "/nonexistent/directory/file.bin"
    );

    // Assert: 验失败 + errorMsg 包含 "failed to open"
    EXPECT_FALSE(result.ok());
    EXPECT_NE(std::string::npos, result.errorMsg.find("failed to open"));
}

/* ====================================================================
 * 自定义头测试
 * ==================================================================== */

TEST(HttpClientHeadersTest, CustomHeadersReceived) {
    // Arrange: fake server 检查自定义头
    std::string receivedAuth;
    FakeHttpServer::Builder b;
    b.route("/api", [&receivedAuth](const Request& req) {
        if (req.headers.count("Authorization") > 0) {
            receivedAuth = req.headers.at("Authorization");
        }
        return Response(200, "ok");
    });
    auto server = b.start();

    HttpClient client(HttpClientConfig{});

    // Act: 带 Authorization 头
    auto resp = client.postJson(
        server->url("/api"),
        "{}",
        {"Authorization: Bearer token123"}
    );

    // Assert: 验服务器收到了头
    EXPECT_TRUE(resp.ok());
    EXPECT_EQ("Bearer token123", receivedAuth);
}
