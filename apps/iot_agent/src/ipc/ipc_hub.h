/**
 * @file ipc_hub.h
 * @brief IPC 中心节点封装 —— ROUTER 模式，等 mediad 等业务进程连上来
 *
 * 封装了什么：
 *   - libmq MessageManager 的创建 / 启动 / 停止（含日志接入）
 *   - ConfigSyncManager 的创建（配置可靠下发给各业务进程）
 *   - 服务心跳：启动后广播心跳请求，收到 ACK 后标记服务在线
 *   - 云端配置变更的翻译与下发（publishConfigDelta）
 *
 * 为什么从 main.cpp 搬出来：
 *   这些是"IPC 通道本身"的搭建逻辑，不是 main 该操心的组装工作。
 *   main 只需要 start() 一个调用，然后拿 manager() 去订阅业务事件。
 *
 * 失败策略：
 *   IPC 启动失败不致命——进程继续跑（云连接还在），只是 mediad 的
 *   事件转发和配置下发用不了。所以 start() 返回 false 时记日志继续。
 */

/**
 * IPC hub node wrapper — ROUTER mode, waits for mediad and other business processes to connect
 *
 * What it wraps:
 *   - libmq MessageManager creation / start / stop (with log integration)
 *   - ConfigSyncManager creation (reliable config delivery to business processes)
 *   - Service heartbeat: broadcast heartbeat requests on start, mark service online on ACK
 *   - Cloud config delta translation and delivery (publishConfigDelta)
 *
 * Why extracted from main.cpp:
 *   These are "IPC channel infrastructure" setup logic, not assembly work for main.
 *   main only needs a single start() call, then uses manager() to subscribe to business events.
 *
 * Failure strategy:
 *   IPC startup failure is non-fatal — the process keeps running (cloud connection is still
 *   alive), only mediad event forwarding and config delivery become unavailable.
 *   So start() logs and continues on false.
 */
#pragma once

#include "libmq/message_manager.h"
#include "nc/config_sync/config_sync_manager.h"

#include <cstdint>
#include <memory>
#include <string>

namespace iot_agent {

class IpcHub {
public:
    IpcHub() = default;
    ~IpcHub();

    IpcHub(const IpcHub&) = delete;
    IpcHub& operator=(const IpcHub&) = delete;

    /**
     * 创建并启动 IPC 中心节点
     *
     * 成功后会：创建 ConfigSyncManager、注册 mediad 服务、
     * 订阅心跳 ACK、广播一次心跳请求。
     *
     * @return true=启动成功；false=失败（进程可继续跑，只是没 IPC）
     */
    bool start();

    /** 停止并释放（析构时自动调用） */
    void stop();

    /** 是否已启动 */
    bool isRunning() const { return m_manager != nullptr; }

    /**
     * 底层消息管理器（订阅业务事件用）
     * 未启动时返回 nullptr，调用方需判空
     */
    libmq::MessageManager* manager() { return m_manager.get(); }

    /**
     * 向指定服务发命令（RPC 指令转发给 mediad 用）
     * IPC 未启动时返回 false
     */
    bool sendTo(const std::string& target, uint32_t type, const std::string& payload);

    /**
     * 把云端配置变更下发给所有在线服务
     *
     * 内部做的事：
     *   1) 把云端扁平字段翻译成 mediad 的嵌套配置结构
     *   2) 交给 ConfigSyncManager，由它向所有在线服务下发
     *
     * @param version 配置版本号（由调用方从云端 JSON 提取）
     * @param delta   ConfigRouter 算出来的变更字段 JSON
     * @return true=已受理；false=IPC 未启动，配置被丢弃
     */
    bool publishConfigDelta(uint64_t version, const std::string& delta);

    /** 设置设备 ID，下发配置时自动注入 device_id 字段给 mediad */
    void setDeviceId(const std::string& id) { m_deviceId = id; }

private:
    std::string m_deviceId;
    std::unique_ptr<libmq::MessageManager> m_manager;
    std::unique_ptr<nc::config_sync::ConfigSyncManager> m_configSync;
};

} // namespace iot_agent
