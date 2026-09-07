/**
 * zmq_context.h
 * lib-mq 全局 ZeroMQ Context 单例
 *
 * 使用 Meyers 单例模式管理全局唯一的 zmq::context_t。
 * C++11 标准保证静态局部变量的初始化是线程安全的。
 * 整个进程只存在一个 context，所有 socket 共享，节省资源。
 *
 * 使用示例：
 *   auto& ctx = ZmqContextManager::instance();
 *   zmq::socket_t sock(ctx, ZMQ_PUB);
 */
#pragma once
#include <zmq.hpp>

namespace libmq {

/**
 * ZeroMQ Context 全局单例管理器
 *
 * 负责创建和管理进程内唯一的 zmq::context_t。
 * 禁止拷贝和移动，确保全局唯一性。
 */
class ZmqContextManager {
public:
    /**
     * 获取全局唯一的 ZeroMQ context
     * 
     * Meyers 单例，C++11 保证线程安全初始化。
     * 首次调用时创建 context，后续调用返回同一实例。
     * 
     * @return 全局 zmq::context_t 引用
     */
    static zmq::context_t& instance() {
        static zmq::context_t ctx(1);  // 1 个 I/O 线程
        return ctx;
    }

    // 禁止拷贝和移动
    ZmqContextManager(const ZmqContextManager&) = delete;
    ZmqContextManager& operator=(const ZmqContextManager&) = delete;
    ZmqContextManager(ZmqContextManager&&) = delete;
    ZmqContextManager& operator=(ZmqContextManager&&) = delete;

private:
    ZmqContextManager() = default;
    ~ZmqContextManager() = default;
};

}
