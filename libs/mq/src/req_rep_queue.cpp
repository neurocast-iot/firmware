/**
 * req_rep_queue.cpp
 * 请求-响应消息队列实现
 */

#include "libmq/req_rep_queue.h"
#include "libmq/zmq_context.h"
#include <iostream>

namespace libmq {

ReqRepQueue::ReqRepQueue() = default;

ReqRepQueue::~ReqRepQueue() = default;

bool ReqRepQueue::initializeAsServer(const std::string& endpoint, const MqConfig& config) {
    endpoint_ = endpoint;
    isServer_ = true;
    logCallback_ = config.logCallback;
    timeoutMs_ = config.recvTimeoutMs > 0 ? config.recvTimeoutMs : 2000;

    try {
        socket_ = std::make_unique<zmq::socket_t>(ZmqContextManager::instance(), ZMQ_REP);
        
        // 设置接收超时
        socket_->set(zmq::sockopt::rcvtimeo, timeoutMs_);
        socket_->set(zmq::sockopt::sndtimeo, timeoutMs_);
        
        socket_->bind(endpoint_);
        initialized_ = true;
        
        log(LogLevel::Info, "ReqRepQueue 服务端已绑定: " + endpoint_);
        return true;
        
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "绑定端点失败: " + endpoint_ + " (" + e.what() + ")");
        socket_.reset();
        return false;
    }
}

bool ReqRepQueue::initializeAsClient(const std::string& endpoint, const MqConfig& config) {
    endpoint_ = endpoint;
    isServer_ = false;
    logCallback_ = config.logCallback;
    timeoutMs_ = config.recvTimeoutMs > 0 ? config.recvTimeoutMs : 2000;

    try {
        socket_ = std::make_unique<zmq::socket_t>(ZmqContextManager::instance(), ZMQ_REQ);
        
        // 设置超时
        socket_->set(zmq::sockopt::rcvtimeo, timeoutMs_);
        socket_->set(zmq::sockopt::sndtimeo, timeoutMs_);
        
        socket_->connect(endpoint_);
        initialized_ = true;
        
        log(LogLevel::Info, "ReqRepQueue 客户端已连接: " + endpoint_);
        return true;
        
    } catch (const zmq::error_t& e) {
        log(LogLevel::Error, "连接端点失败: " + endpoint_ + " (" + e.what() + ")");
        socket_.reset();
        return false;
    }
}

std::string ReqRepQueue::recvRequest(int timeoutMs) {
    if (!initialized_ || !isServer_ || !socket_) {
        return "";
    }

    try {
        zmq::message_t msg;
        auto result = socket_->recv(msg, zmq::recv_flags::none);
        
        if (result) {
            std::string request(static_cast<char*>(msg.data()), msg.size());
            return request;
        }
        
        return "";  // 超时或错误
        
    } catch (const zmq::error_t& e) {
        if (e.num() != EAGAIN) {
            log(LogLevel::Error, "接收请求失败: " + std::string(e.what()));
        }
        return "";
    }
}

MqResult ReqRepQueue::sendResponse(const std::string& response) {
    if (!initialized_ || !isServer_ || !socket_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "队列未初始化");
    }

    try {
        zmq::message_t msg(response.size());
        memcpy(msg.data(), response.c_str(), response.size());
        socket_->send(msg, zmq::send_flags::none);
        
        return MqResult::success();
        
    } catch (const zmq::error_t& e) {
        return MqResult::fail(MqErrorCode::SendFailed, "发送响应失败: " + std::string(e.what()));
    }
}

MqResult ReqRepQueue::request(const std::string& request, int timeoutMs) {
    if (!initialized_ || isServer_ || !socket_) {
        return MqResult::fail(MqErrorCode::NotInitialized, "队列未初始化或模式错误");
    }

    try {
        // 发送请求
        zmq::message_t reqMsg(request.size());
        memcpy(reqMsg.data(), request.c_str(), request.size());
        socket_->send(reqMsg, zmq::send_flags::none);
        
        // 接收响应
        zmq::message_t replyMsg;
        auto result = socket_->recv(replyMsg, zmq::recv_flags::none);
        
        if (result) {
            std::string response(static_cast<char*>(replyMsg.data()), replyMsg.size());
            return MqResult::success(response);
        }
        
        return MqResult::fail(MqErrorCode::Timeout, "接收响应超时");
        
    } catch (const zmq::error_t& e) {
        // 捕获 EFSM 错误: REQ socket 状态机损坏,自动重建
        if (e.num() == EFSM) {
            log(LogLevel::Warn, "REQ socket EFSM错误，重建socket: " + endpoint_);
            
            // 关闭旧 socket
            socket_.reset();
            
            // 创建新的 REQ socket
            try {
                socket_ = std::make_unique<zmq::socket_t>(ZmqContextManager::instance(), ZMQ_REQ);
                socket_->set(zmq::sockopt::rcvtimeo, timeoutMs_);
                socket_->set(zmq::sockopt::sndtimeo, timeoutMs_);
                socket_->connect(endpoint_);
                
                log(LogLevel::Info, "REQ socket 重建成功: " + endpoint_);
            } catch (const zmq::error_t& rebuild_err) {
                log(LogLevel::Error, "REQ socket 重建失败: " + std::string(rebuild_err.what()));
                socket_.reset();
            }
            
            return MqResult::fail(MqErrorCode::EFSMError, "EFSM错误，已尝试重建socket: " + std::string(e.what()));
        }
        
        return MqResult::fail(MqErrorCode::RecvFailed, "请求失败: " + std::string(e.what()));
    }
}

void ReqRepQueue::log(LogLevel level, const std::string& msg) {
    if (logCallback_) {
        logCallback_(level, msg);
    }
}

} // namespace libmq
