/**
 * @file event_dispatcher_impl.cpp
 * @brief EventDispatcher 实现（观察者模式）
 *
 * 线程安全策略：
 *   - subscribe/unsubscribe：加互斥锁修改订阅列表
 *   - dispatch：先复制当前订阅快照，释放锁后逐一调用回调
 *     （避免回调中调用 unsubscribe 导致死锁）
 *   - 待删除列表：dispatch 期间收集的 unsubscribe 请求，
 *     dispatch 完成后统一清理
 */
#include "camera/event_dispatcher.h"
#include <algorithm>

namespace camera {

/**
 * @brief 注册帧回调
 */
SubscriptionId EventDispatcher::subscribe(ChannelId     channel,
                                          FrameCallback callback) {
    if (!callback) {
        return kInvalidSubscriptionId;
    }

    /* 生成唯一订阅 ID（原子自增，无需加锁） */
    SubscriptionId id = m_nextId.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_subscriptions[id] = Subscription{id, channel, std::move(callback)};
    return id;
}

/**
 * @brief 取消订阅
 *
 * 若当前正在 dispatch，则将 id 加入待删除列表，dispatch 结束后清理。
 * 否则立即删除。
 */
void EventDispatcher::unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_dispatching) {
        /* dispatch 中：加入延迟删除列表，避免迭代器失效 */
        m_pendingRemove.push_back(id);
    } else {
        m_subscriptions.erase(id);
    }
}

/**
 * @brief 取消指定通道的所有订阅
 */
void EventDispatcher::unsubscribeAll(ChannelId channel) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_subscriptions.begin();
    while (it != m_subscriptions.end()) {
        if (it->second.channel == channel) {
            if (m_dispatching) {
                m_pendingRemove.push_back(it->first);
                ++it;
            } else {
                it = m_subscriptions.erase(it);
            }
        } else {
            ++it;
        }
    }
}

/**
 * @brief 清除所有订阅
 */
void EventDispatcher::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_subscriptions.clear();
    m_pendingRemove.clear();
}

/**
 * @brief 分发帧给所有匹配通道的订阅者
 *
 * 实现策略：
 *   1. 加锁，复制当前订阅快照（vector<Callback>）
 *   2. 设置 m_dispatching = true
 *   3. 解锁，使用快照逐一调用回调
 *      （解锁期间 subscribe/unsubscribe 可并发执行）
 *   4. 加锁，清理 pendingRemove 列表
 *   5. 清除 m_dispatching 标志
 */
void EventDispatcher::dispatch(const VideoFrame& frame) {
    /* 按通道筛选，复制当前回调快照 */
    std::vector<FrameCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dispatching = true;

        callbacks.reserve(m_subscriptions.size());
        for (auto& pair : m_subscriptions) {
            if (pair.second.channel == frame.channel()) {
                callbacks.push_back(pair.second.callback);
            }
        }
    }

    /* 无锁调用回调，允许回调中并发 subscribe/unsubscribe */
    for (auto& cb : callbacks) {
        cb(frame);
    }

    /* 清理 dispatch 期间积累的待删除请求 */
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (SubscriptionId id : m_pendingRemove) {
            m_subscriptions.erase(id);
        }
        m_pendingRemove.clear();
        m_dispatching = false;
    }
}

/**
 * @brief 获取指定通道的订阅者数量
 */
size_t EventDispatcher::subscriberCount(ChannelId channel) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (auto& pair : m_subscriptions) {
        if (pair.second.channel == channel) {
            ++count;
        }
    }
    return count;
}

/**
 * @brief 是否有任意订阅者
 */
bool EventDispatcher::hasSubscribers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_subscriptions.empty();
}

/**
 * @brief 检查指定通道是否有订阅者
 */
bool EventDispatcher::hasSubscribers(ChannelId channel) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_subscriptions) {
        if (pair.second.channel == channel) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 根据订阅 ID 获取通道 ID
 */
ChannelId EventDispatcher::getChannelId(SubscriptionId id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_subscriptions.find(id);
    if (it != m_subscriptions.end()) {
        return it->second.channel;
    }
    return ChannelId::Main;  // 默认返回值
}

} // namespace camera
