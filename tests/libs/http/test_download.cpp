/**
 * @file test_download.cpp
 * @brief nc::http::downloadWithPolicy 端到端测试：用 fake HTTP server 验证成功/重试/404
 *
 * 测什么:
 *   - 一次成功 (Success)
 *   - 重试后成功 (RetryThenSuccess, 验重试逻辑生效)
 *   - 全部重试都失败 (AllRetriesFail, 验 attempts 计数对)
 *   - 资源不存在 (NotFound, 验 HTTP 错误传递)
 *   - 外部取消 (CancelBeforeDownload, 验 cancel_flag 早返回)
 *
 * 不测：断点续传（HttpClient 自己的功能，不在 DownloadPolicy 范围内）
 *       大文件（fake server 用小数据就行；性能靠 HttpClient 测）
 */
#include "nc/http/download.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "fake_http_server.h"

using nc::http::downloadWithPolicy;
using nc::http::DownloadPolicy;
using nc::ota_test::FakeHttpServer;

/* 工具：把 string 内容写到临时文件 */
static std::string writeTempFile(const std::string& content) {
    char tmpl[] = "/tmp/dlpkg_XXXXXX";
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

TEST(DownloadWithPolicyTest, Success) {
    /* fake server: GET /file.bin → 200 + "hello" */
    const std::string payload = "hello";
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&](const auto&) {
        return nc::ota_test::Response(200, payload);
    });
    auto server = b.start();

    DownloadPolicy policy;
    policy.max_retries = 0;   /* 不重试，看第一次就成功 */
    policy.disk_check = false; /* 路径不存在时 statvfs 会失败 */

    auto savePath = writeTempFile("");
    auto r = downloadWithPolicy(server->url("/file.bin"), savePath, policy);
    EXPECT_TRUE(r.ok) << r.errorMsg;
    EXPECT_EQ(payload, readFile(savePath));
    EXPECT_EQ(1, r.attempts);

    ::std::remove(savePath.c_str());
}

TEST(DownloadWithPolicyTest, RetryThenSuccess) {
    /* fake server: 前 2 次返回 500，第 3 次返回 200 + "retry ok" */
    std::atomic<int> hits{0};
    const std::string payload = "retry ok";
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&](const auto&) {
        int n = hits.fetch_add(1) + 1;
        if (n < 3) return nc::ota_test::Response(500, "server error");
        return nc::ota_test::Response(200, payload);
    });
    auto server = b.start();

    DownloadPolicy policy;
    policy.max_retries = 3;
    policy.retry_delay_sec = 0;   /* 测试不 sleep 节省时间 */
    policy.disk_check = false;
    policy.resume_support = false; /* 关掉续传探测, 避免 HttpClient probe 多发 1 个请求 */

    auto savePath = writeTempFile("");
    auto r = downloadWithPolicy(server->url("/file.bin"), savePath, policy);
    EXPECT_TRUE(r.ok) << r.errorMsg;
    EXPECT_EQ(payload, readFile(savePath));
    EXPECT_EQ(3, r.attempts);   /* 失败 2 次 + 成功 1 次 = 3 次尝试 */
    EXPECT_EQ(3, server->requestCount());

    ::std::remove(savePath.c_str());
}

TEST(DownloadWithPolicyTest, AllRetriesFail) {
    /* fake server: 一直返回 500 */
    std::atomic<int> hits{0};
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&](const auto&) {
        hits.fetch_add(1);
        return nc::ota_test::Response(500, "always fail");
    });
    auto server = b.start();

    DownloadPolicy policy;
    policy.max_retries = 2;     /* 1 + 2 = 3 次都失败 */
    policy.retry_delay_sec = 0;
    policy.disk_check = false;

    auto savePath = writeTempFile("");
    auto r = downloadWithPolicy(server->url("/file.bin"), savePath, policy);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(3, r.attempts);
    EXPECT_FALSE(r.errorMsg.empty());
    EXPECT_EQ(3, server->requestCount());

    ::std::remove(savePath.c_str());
}

TEST(DownloadWithPolicyTest, NotFound) {
    /* 没有 register /missing → 默认 404 */
    FakeHttpServer::Builder b;  /* 空路由 */
    auto server = b.start();

    DownloadPolicy policy;
    policy.disk_check = false;
    policy.max_retries = 0;        /* 1 次不重试 (避免默认重试 3 次等 15s) */
    policy.retry_delay_sec = 0;    /* 测试不 sleep */

    auto savePath = writeTempFile("");
    auto r = downloadWithPolicy(server->url("/missing"), savePath, policy);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(1, r.attempts);  /* 不重试 1 次就返回 */
    EXPECT_EQ(1, server->requestCount());

    ::std::remove(savePath.c_str());
}

TEST(DownloadWithPolicyTest, CancelBeforeDownload) {
    /* fake server：能成功，但 cancel_flag 在下载前就置 true */
    FakeHttpServer::Builder b;
    b.route("/file.bin", [&](const auto&) {
        return nc::ota_test::Response(200, "should not reach");
    });
    auto server = b.start();

    std::atomic<bool> cancel{true};  /* 起始就取消 */

    DownloadPolicy policy;
    policy.disk_check = false;

    auto savePath = writeTempFile("");
    auto r = downloadWithPolicy(server->url("/file.bin"), savePath, policy,
                                nullptr, nullptr, &cancel);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.errorMsg.find("cancel"));

    ::std::remove(savePath.c_str());
}
