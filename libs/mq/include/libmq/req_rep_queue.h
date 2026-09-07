/**
 * req_rep_queue.h
 * 请求-响应消息队列
 *
 * 使用 ZeroMQ 的 REQ-REP 模式,实现同步的请求-响应通信。
 * 适用于监控查询、配置获取等需要即时响应的场景。
 *
 * 特点:
 * - 同步通信: 发送请求后阻塞等待响应
 * - 严格交替: 必须先发送请求,才能接收响应
 * - 超时控制: 支持发送/接收超时设置
 *
 * 使用示例:
 *   // 服务端 (被监控进程,如 iot_live)
 *   ReqRepQueue server;
 *   server.initializeAsServer("ipc:///tmp/iot_live_status.ipc");
 *   server.start();
 *   
 *   // 接收请求并响应
 *   std::string request = server.recvRequest(2000);  // 2秒超时
 *   std::string response = buildStatusJson();
 *   server.sendResponse(response);
 *
 *   // 客户端 (监控进程,如 iot_monitor)
 *   ReqRepQueue client;
 *   client.initializeAsClient("ipc:///tmp/iot_live_status.ipc");
 *   
 *   // 发送请求并接收响应
 *   auto result = client.request("GET_STATUS", 2000);  // 2秒超时
 */
#pragma once
#include <string>
#include <memory>
#include <zmq.hpp>
#include "libmq/config.h"
#include "libmq/result.h"

namespace libmq {

/**
 * 请求-响应队列类
 *
 * 封装 ZeroMQ REQ-REP 传输,提供同步请求-响应接口。
 * 支持服务端(REP)和客户端(REQ)两种模式。
 */
class ReqRepQueue {
public:
    /**
     * 构造函数
     */
    ReqRepQueue();

    /**
     * 析构函数
     */
    ~ReqRepQueue();

    /**
     * 初始化服务端 (REP 模式)
     *
     * 绑定到指定 IPC 端点,等待客户端请求。
     *
     * @param endpoint IPC 端点地址,格式为 "ipc://socket文件路径"
     * @param config 配置参数(可选)
     * @return true=初始化成功, false=端点已被占用
     */
    bool initializeAsServer(const std::string& endpoint, const MqConfig& config = MqConfig{});

    /**
     * 初始化客户端 (REQ 模式)
     *
     * 连接到服务端创建的 IPC 端点。
     *
     * @param endpoint IPC 端点地址
     * @param config 配置参数(可选)
     * @return true=初始化成功, false=服务端未启动
     */
    bool initializeAsClient(const std::string& endpoint, const MqConfig& config = MqConfig{});

    /**
     * 接收请求 (仅服务端)
     *
     * 阻塞等待客户端请求,超时返回空字符串。
     *
     * @param timeoutMs 超时时间(毫秒)
     * @return 请求内容,超时或错误返回空字符串
     */
    std::string recvRequest(int timeoutMs = 2000);

    /**
     * 发送响应 (仅服务端)
     *
     * 向客户端发送响应数据。
     *
     * @param response 响应内容
     * @return MqResult 操作结果
     */
    MqResult sendResponse(const std::string& response);

    /**
     * 发送请求并接收响应 (仅客户端)
     *
     * 同步操作: 发送请求后阻塞等待响应。
     *
     * @param request 请求内容
     * @param timeoutMs 总超时时间(毫秒,包含发送和接收)
     * @return MqResult 操作结果,成功时 result.value 包含响应内容
     */
    MqResult request(const std::string& request, int timeoutMs = 2000);

    /** 检查是否为服务端模式 */
    bool isServer() const { return isServer_; }

    /** 检查队列是否已初始化 */
    bool isInitialized() const { return initialized_; }

private:
    /**
     * 输出日志
     */
    void log(LogLevel level, const std::string& msg);

    std::string endpoint_;
    bool isServer_ = false;
    std::unique_ptr<zmq::socket_t> socket_;
    bool initialized_ = false;

    MqConfig::LogCallback logCallback_;
    int timeoutMs_ = 2000;
};

} // namespace libmq
