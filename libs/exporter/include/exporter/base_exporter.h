/**
 * base_exporter.h
 * 进程监控导出器基类
 * 
 * 提供进程监控指标的 ZeroMQ REQ-REP 服务接口:
 * - 自动采集通用进程指标 (从 /proc 读取)
 * - 支持注册自定义业务指标 (通过回调函数)
 * - 响应监控中心的查询请求
 * 
 * 特点:
 * - 极简集成: 只需 3 行代码即可为任意进程添加监控支持
 * - 零配置: 自动采集 12+ 个通用指标,无需手动配置
 * - 可扩展: 通过回调函数注册任意自定义业务指标
 * - 轻量级: 基于 lib-mq 的 REQ-REP 模式,延迟 < 100μs
 * 
 * 使用示例:
 *   // 步骤 1: 创建导出器
 *   auto exporter = BaseExporter::create("iot_live", "ipc:///tmp/iot_live_status.ipc");
 *   
 *   // 步骤 2: 注册自定义业务指标 (可选)
 *   exporter->registerMetric("rtmp_connections", []() {
 *       return static_cast<double>(getRTMPConnectionCount());
 *   });
 *   
 *   // 步骤 3: 启动导出器
 *   exporter->start();
 *   
 *   // 监控中心 (如 iot_monitor) 可以发送查询请求:
 *   // 响应 JSON: {"pid":1234, "process":"iot_live", "memory":{"rss_kb":52480}, ...}
 */

#ifndef LIBEXPORTER_BASE_EXPORTER_H
#define LIBEXPORTER_BASE_EXPORTER_H

#include "exporter/config.h"
#include "exporter/metrics_collector.h"
#include "libmq/req_rep_queue.h"

#include <string>
#include <memory>
#include <functional>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>

namespace exporter {

/**
 * 指标回调函数类型
 * 返回 double 类型的指标值
 */
using MetricCallback = std::function<double()>;

/**
 * 导出器主类
 */
class BaseExporter {
public:
    /**
     * 创建导出器 (工厂方法)
     * @param process_name 进程名称
     * @param endpoint ZeroMQ IPC 端点
     * @return BaseExporter 智能指针
     */
    static std::unique_ptr<BaseExporter> create(
        const std::string& process_name,
        const std::string& endpoint
    );
    
    /**
     * 构造函数
     * @param config 导出器配置
     */
    explicit BaseExporter(const ExporterConfig& config);
    
    /**
     * 析构函数 (停止后台线程)
     */
    ~BaseExporter();
    
    // 禁止拷贝
    BaseExporter(const BaseExporter&) = delete;
    BaseExporter& operator=(const BaseExporter&) = delete;
    
    /**
     * 注册自定义指标
     * @param name 指标名称
     * @param callback 采集回调函数
     */
    void registerMetric(const std::string& name, MetricCallback callback);
    
    /**
     * 启动导出器 (后台线程)
     */
    void start();
    
    /**
     * 停止导出器
     */
    void stop();
    
    /**
     * 手动处理一次请求 (阻塞模式,可选)
     */
    void handleOneRequest();

private:
    // 后台线程主循环
    void runServerLoop();
    
    // 构建 JSON 响应
    std::string buildResponse();
    
    // 日志输出
    void log(int level, const std::string& msg);
    
    // 配置
    ExporterConfig config_;
    
    // lib-mq ReqRepQueue
    std::unique_ptr<libmq::ReqRepQueue> req_rep_queue_;
    
    // 自定义指标
    std::map<std::string, MetricCallback> custom_metrics_;
    std::mutex metrics_mutex_;
    
    // 运行状态
    std::atomic<bool> running_;
    std::thread server_thread_;
    
    // CPU 采样历史 (用于计算 CPU 使用率)
    unsigned long last_cpu_time_ = 0;
    time_t last_cpu_collect_time_ = 0;
};

} // namespace exporter

#endif // LIBEXPORTER_BASE_EXPORTER_H
