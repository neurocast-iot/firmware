/**
 * @file fake_http_server.cpp
 * @brief libmicrohttpd 回调 → FakeHttpServer::handle 路由
 *
 * 故意写得简单：每个 path 一个 handler，不做正则匹配，
 * 测什么路由就 register 什么。生产场景的复杂路由不是测试该干的事。
 */
#include "fake_http_server.h"

#include <cstring>
#include <microhttpd.h>
#include <memory>
#include <stdexcept>

namespace nc {
namespace ota_test {

/* ---- MHD 回调：收到请求 → 组装 Request → 调 handler → 回 Response ---- */
static enum MHD_Result request_cb(void* cls,
                                  struct MHD_Connection* connection,
                                  const char* url,
                                  const char* method,
                                  const char* version,
                                  const char* upload_data,
                                  size_t* upload_data_size,
                                  void** con_cls) {
    /* MHD 二次回调机制：第一次不带 body（con_cls == NULL），第二次带 body */
    static_cast<void>(version);
    auto* server = static_cast<FakeHttpServer*>(cls);
    auto* state = static_cast<Request*>(*con_cls);

    if (state == nullptr) {
        /* 第一次回调：建 Request 存到 con_cls */
        auto req = new Request();
        req->method = method ? method : "";
        req->path = url ? url : "";

        /* 解析 query string（?key=val） */
        const char* q = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
        static_cast<void>(q);  /* 不真用，留扩展 */

        /* headers：遍历所有请求头 */
        MHD_get_connection_values(
            connection, MHD_HEADER_KIND,
            [](void* cls, enum MHD_ValueKind,
               const char* key, const char* value) -> enum MHD_Result {
                auto* headers = static_cast<std::map<std::string, std::string>*>(cls);
                if (key && value) {
                    (*headers)[key] = value;
                }
                return MHD_YES;
            }, &req->headers);

        *con_cls = req;
        return MHD_YES;  /* 告诉 MHD 继续，准备收 body */
    }

    /* 第二次回调：带 body 数据 */
    if (*upload_data_size != 0) {
        state->body.insert(state->body.end(),
                           upload_data, upload_data + *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* body 收完了，调 handler 出 response */
    Response resp = server->handle(*state);
    server->bumpRequestCount();  // 内部计数，供测试断言用

    struct MHD_Response* mhd_resp = MHD_create_response_from_buffer(
        resp.body.size(),
        const_cast<void*>(static_cast<const void*>(resp.body.data())),
        MHD_RESPMEM_MUST_COPY);
    if (!mhd_resp) {
        delete state;
        *con_cls = nullptr;
        return MHD_NO;
    }
    for (const auto& [k, v] : resp.headers) {
        MHD_add_response_header(mhd_resp, k.c_str(), v.c_str());
    }
    /* Content-Length MHD 会自动算 */
    enum MHD_Result ret = MHD_queue_response(connection,
                                             static_cast<unsigned int>(resp.status),
                                             mhd_resp);
    MHD_destroy_response(mhd_resp);
    delete state;
    *con_cls = nullptr;
    return ret;
}

/* ---- FakeHttpServer 实现 ---- */
void FakeHttpServer::installRoutes(std::map<std::string, RouteHandler> routes) {
    routes_ = std::move(routes);
}

Response FakeHttpServer::handle(const Request& req) {
    auto it = routes_.find(req.path);
    if (it == routes_.end()) {
        return Response(404, "not found: " + req.path);
    }
    return it->second(req);
}

std::unique_ptr<FakeHttpServer> FakeHttpServer::Builder::start() {
    auto s = std::unique_ptr<FakeHttpServer>(new FakeHttpServer());
    s->installRoutes(std::move(routes_));

    s->daemon_ = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,  /* 内部线程，省事 */
        0,                                /* port=0：系统自动分配 */
        nullptr, nullptr,
        &request_cb, s.get(),
        MHD_OPTION_END);
    if (!s->daemon_) {
        throw std::runtime_error("MHD_start_daemon failed");
    }

    /* 问 MHD 我们绑的端口 */
    const union MHD_DaemonInfo* info = MHD_get_daemon_info(
        s->daemon_, MHD_DAEMON_INFO_BIND_PORT);
    if (!info) {
        MHD_stop_daemon(s->daemon_);
        s->daemon_ = nullptr;
        throw std::runtime_error("MHD_get_daemon_info failed");
    }
    s->port_ = static_cast<uint16_t>(info->port);
    return s;
}

std::string FakeHttpServer::url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
}

void FakeHttpServer::stop() {
    if (daemon_) {
        MHD_stop_daemon(daemon_);
        daemon_ = nullptr;
    }
}

FakeHttpServer::~FakeHttpServer() {
    stop();
}

} // namespace ota_test
} // namespace nc
