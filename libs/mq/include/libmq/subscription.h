/**
 * subscription.h
 * lib-mq RAII 订阅句柄
 *
 * 管理消息订阅的生命周期。subscribe() 返回此对象，离开作用域时自动取消订阅。
 * 可移动，不可拷贝。防止忘记取消订阅导致的内存泄漏。
 *
 * 使用示例：
 *   {
 *       auto sub = manager.subscribe(type, callback);
 *       // ... 处理消息
 *   } // sub 析构，自动 unsubscribe
 *
 *   // 也可以手动取消
 *   sub.unsubscribe();
 */
#pragma once
#include <functional>

namespace libmq {

/**
 * RAII 订阅句柄
 *
 * 封装取消订阅逻辑，对象销毁时自动调用取消订阅函数。
 * 支持移动语义，便于从函数返回或存入容器。
 */
class Subscription {
public:
    using UnsubscribeFunc = std::function<void()>;

    /**
     * 默认构造函数
     *
     * 创建一个空的订阅句柄（不关联任何取消订阅函数）。
     */
    Subscription() = default;

    /**
     * 构造函数
     * 
     * @param func 取消订阅函数，在析构或手动 unsubscribe 时调用
     */
    explicit Subscription(UnsubscribeFunc func)
        : unsubscribeFunc_(std::move(func)) {}

    /**
     * 析构函数
     *
     * 如果订阅仍然活跃，自动调用取消订阅函数。
     */
    ~Subscription() { unsubscribe(); }

    /**
     * 移动构造函数
     *
     * 转移订阅所有权，原对象变为空句柄。
     */
    Subscription(Subscription&& other) noexcept
        : unsubscribeFunc_(std::move(other.unsubscribeFunc_)) {
        other.unsubscribeFunc_ = nullptr;
    }

    /**
     * 移动赋值运算符
     *
     * 先取消当前订阅（如果有），再转移新订阅的所有权。
     */
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            unsubscribe();
            unsubscribeFunc_ = std::move(other.unsubscribeFunc_);
            other.unsubscribeFunc_ = nullptr;
        }
        return *this;
    }

    // 禁止拷贝（订阅句柄应该是唯一的）
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    /**
     * 手动取消订阅
     *
     * 如果订阅仍然活跃，立即取消并清空取消订阅函数。
     * 可多次调用，重复调用无副作用。
     */
    void unsubscribe() {
        if (unsubscribeFunc_) {
            unsubscribeFunc_();
            unsubscribeFunc_ = nullptr;
        }
    }

    /**
     * 检查订阅是否仍然活跃
     * 
     * @return true=订阅活跃, false=已取消或为空句柄
     */
    bool isActive() const { return unsubscribeFunc_ != nullptr; }

private:
    UnsubscribeFunc unsubscribeFunc_;
};

}
