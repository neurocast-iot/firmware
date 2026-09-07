/**
 * @file fake_http_server.h
 * @brief 测试用 fake HTTP server（libmicrohttpd 包装）
 *
 * 用法：
 *   FakeHttpServer::Builder b;
 *   b.route("/file.bin", [&](const Request& req) {
 *       // req.path / req.method / req.headers / req.query
 *       Response resp(200, "hello");
 *       resp.headers["Content-Type"] = "text/plain";
 *       return resp;
 *   });
 *   auto server = b.start();
 *   // ... 用 server.url("/file.bin") 拼 URL 让 HttpClient 访问
 *   server.stop();
 *
 * 设计：每个测试 case 启一个 server，case 结束自动停。
 * 支持的最小特性：GET、Content-Length、Range 头（206 Partial Content）、
 * 状态码可控（测试 4xx/5xx 错误路径用）。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct MHD_Daemon;   // 前置声明，不在头里拉 microhttpd.h

namespace nc {
namespace ota_test {

struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;   // 全小写 key
    std::vector<uint8_t> body;
};

struct Response {
    int status = 200;
    std::vector<uint8_t> body;
    std::map<std::string, std::string> headers;

    Response() = default;
    explicit Response(int s) : status(s) {}
    Response(int s, std::string text)
        : status(s), body(text.begin(), text.end()) {
        headers["Content-Type"] = "text/plain";
    }
    Response(int s, std::vector<uint8_t> bin)
        : status(s), body(std::move(bin)) {
        headers["Content-Type"] = "application/octet-stream";
    }
};

using RouteHandler = std::function<Response(const Request&)>;

class FakeHttpServer {
public:
    class Builder {
    public:
        Builder& route(const std::string& path, RouteHandler h) {
            routes_[path] = std::move(h);
            return *this;
        }
        /** 启动 server，绑定到 127.0.0.1:0（系统自动分配端口） */
        std::unique_ptr<FakeHttpServer> start();
    private:
        std::map<std::string, RouteHandler> routes_;
    };

    ~FakeHttpServer();

    FakeHttpServer(const FakeHttpServer&) = delete;
    FakeHttpServer& operator=(const FakeHttpServer&) = delete;

    /** 拼接完整 URL：http://127.0.0.1:<port><path> */
    std::string url(const std::string& path) const;

    /** 获取实际监听的端口 */
    uint16_t port() const { return port_; }

    /** 停止 server（析构会自动调，显式调也行） */
    void stop();

    /** 累计收到的请求数（测试断言用） */
    int requestCount() const { return request_count_; }

    /**
     * 调用路由处理（libmicrohttpd 回调里调这个）
     * 不暴露给用户，只给 fake_http_server.cpp 用
     */
    Response handle(const Request& req);

    /** 累加请求计数（static 回调里调，故公开） */
    void bumpRequestCount() { ++request_count_; }

private:
    friend class Builder;
    FakeHttpServer() = default;
    void installRoutes(std::map<std::string, RouteHandler> routes);

    MHD_Daemon* daemon_ = nullptr;
    uint16_t port_ = 0;
    int request_count_ = 0;
    std::map<std::string, RouteHandler> routes_;
};

} // namespace ota_test
} // namespace nc
