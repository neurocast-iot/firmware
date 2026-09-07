/**
 * @file event_dispatcher.h
 * @brief 事件分发器（观察者模式核心）
 *
 * EventDispatcher 实现一对多的帧事件分发机制：
 *   - 支持按通道 (ChannelId) 订阅
 *   - 支持多个订阅者同时接收同一通道的帧
 *   - 线程安全（采集线程 dispatch，用户线程 subscribe/unsubscribe）
 *   - 使用自增整数作为订阅 ID，便于精确取消订阅
 *
 * 设计参考：
 *   - Qt Signal/Slot 思想（但不依赖 Qt）
 *   - libcamera RequestCompleted 回调模型
 */
#pragma once

#include "types.h"
#include "frame.h"
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <vector>

namespace camera {

/**
 * @brief 订阅 ID 类型（用于取消订阅）
 */
using SubscriptionId = int32_t;

/** 无效订阅 ID */
constexpr SubscriptionId kInvalidSubscriptionId = -1;

/**
 * @brief 帧回调函数类型
 *
 * 参数为 const VideoFrame&，用户不持有帧所有权。
 * 回调执行完毕后，帧自动析构并释放底层资源。
 */
using FrameCallback = std::function<void(const VideoFrame&)>;

/**
 * @brief 事件分发器
 *
 * 典型用法：
 * @code
 * EventDispatcher dispatcher;
 *
 * // 订阅主通道帧
 * auto id = dispatcher.subscribe(ChannelId::Main, [](const VideoFrame& f) {
 *     sendToRtmp(f.data(), f.size());
 * });
 *
 * // 取消订阅
 * dispatcher.unsubscribe(id);
 * @endcode
 */
class EventDispatcher {
public:
    EventDispatcher()  = default;
    ~EventDispatcher() = default;

    // 不可拷贝
    EventDispatcher(const EventDispatcher&)            = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;

    // --------------------------------------------------------
    // 订阅管理
    // --------------------------------------------------------

    /**
     * @brief 注册帧回调
     *
     * @param channel  订阅的通道（Main 或 Sub）
     * @param callback 帧到达时触发的回调函数
     * @return         订阅 ID（用于取消订阅），失败返回 kInvalidSubscriptionId
     *
     * 线程安全：可在任意线程调用。
     */
    SubscriptionId subscribe(ChannelId channel, FrameCallback callback);

    /**
     * @brief 取消订阅
     *
     * @param id 由 subscribe() 返回的订阅 ID
     *
     * 线程安全：可在任意线程调用。
     * 若 id 不存在，静默忽略。
     */
    void unsubscribe(SubscriptionId id);

    /**
     * @brief 取消指定通道的所有订阅
     *
     * @param channel 目标通道
     */
    void unsubscribeAll(ChannelId channel);

    /**
     * @brief 清除所有订阅
     */
    void clear();

    // --------------------------------------------------------
    // 分发（由采集线程调用）
    // --------------------------------------------------------

    /**
     * @brief 分发帧给所有订阅者
     *
     * @param frame 待分发的帧（在所有订阅者回调执行期间有效）
     *
     * 注意：
     *   - 在采集线程中同步调用订阅者回调
     *   - 回调执行期间持有读锁，subscribe/unsubscribe 会被阻塞
     *   - 若回调中调用 unsubscribe(本id)，会在当前 dispatch 完成后生效
     */
    void dispatch(const VideoFrame& frame);

    // --------------------------------------------------------
    // 查询
    // --------------------------------------------------------

    /**
     * @brief 获取指定通道的订阅者数量
     */
    size_t subscriberCount(ChannelId channel) const;

    /**
     * @brief 是否有任意订阅者
     */
    bool hasSubscribers() const;

    /**
     * @brief 检查指定通道是否有订阅者
     * @param channel 目标通道
     * @return true 如果有至少一个订阅者
     */
    bool hasSubscribers(ChannelId channel) const;

    /**
     * @brief 根据订阅 ID 获取通道 ID
     * @param id 订阅 ID
     * @return 通道 ID，如果订阅 ID 无效返回 ChannelId::Main
     */
    ChannelId getChannelId(SubscriptionId id) const;

private:
    /**
     * @brief 订阅条目
     */
    struct Subscription {
        SubscriptionId id;        ///< 订阅 ID
        ChannelId      channel;   ///< 目标通道
        FrameCallback  callback;  ///< 回调函数
    };

    mutable std::mutex           m_mutex;        ///< 保护订阅列表
    std::map<SubscriptionId,
             Subscription>       m_subscriptions; ///< id -> 订阅条目

    std::atomic<SubscriptionId>  m_nextId{0};     ///< 自增订阅 ID

    /**
     * @brief 待删除订阅 ID（dispatch 期间积累，完成后统一清理）
     * 避免回调中调用 unsubscribe 引发迭代器失效
     */
    std::vector<SubscriptionId>  m_pendingRemove;
    bool                         m_dispatching = false;  ///< 是否正在分发
};

} // namespace camera
