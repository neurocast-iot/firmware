/**
 * base_exporter.cpp
 * 监控导出器基类实现
 * 
 * 基于 lib-mq 的 ReqRepQueue 提供监控查询服务。
 * 
 * 核心功能:
 * - 初始化 REQ-REP 服务端,监听 IPC 端点
 * - 后台线程循环处理监控查询请求
 * - 自动采集通用进程指标 (通过 MetricsCollector)
 * - 调用回调函数采集自定义业务指标
 * - 构建 JSON 响应并返回给监控中心
 * 
 * 线程模型:
 * - runServerLoop() 在独立后台线程运行
 * - registerMetric() 在主线程调用,通过 mutex 保护
 * - buildResponse() 在后台线程调用,可能触发用户回调
 */

#include "exporter/base_exporter.h"
#include <cJSON.h>
#include <iostream>
#include <chrono>
#include <unistd.h>

namespace exporter {

/**
 * 工厂方法: 创建导出器实例
 * 
 * @param process_name 进程名称 (用于标识,会包含在 JSON 响应中)
 * @param endpoint ZeroMQ IPC 端点 (如 "ipc:///tmp/iot_live_status.ipc")
 * @return BaseExporter 智能指针
 */
std::unique_ptr<BaseExporter> BaseExporter::create(
    const std::string& process_name,
    const std::string& endpoint
) {
    ExporterConfig config(process_name, endpoint);
    return std::make_unique<BaseExporter>(config);
}

/**
 * 构造函数: 初始化导出器配置
 * 
 * @param config 导出器配置 (进程名称、IPC 端点、日志级别)
 */
BaseExporter::BaseExporter(const ExporterConfig& config)
    : config_(config),
      req_rep_queue_(nullptr),
      running_(false),
      last_cpu_time_(0),
      last_cpu_collect_time_(0) {
}

/**
 * 析构函数: 停止后台线程并清理资源
 */
BaseExporter::~BaseExporter() {
    stop();
}

/**
 * 注册自定义业务指标
 * 
 * 用户通过回调函数注册自定义指标,该指标会附加到 JSON 响应中。
 * 回调函数在后台线程执行,应保持轻量,避免阻塞。
 * 
 * @param name 指标名称 (会在 JSON 中作为 key)
 * @param callback 采集回调函数,返回 double 类型的指标值
 */
void BaseExporter::registerMetric(const std::string& name, MetricCallback callback) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    custom_metrics_[name] = callback;
    log(1, "注册自定义指标: " + name);
}

/**
 * 启动导出器 (后台线程)
 * 
 * 初始化 lib-mq ReqRepQueue 服务端,启动后台线程循环处理查询请求。
 * 将用户的日志回调传递到 lib-mq,保持日志机制一致。
 * 如果已经在运行,则输出警告并返回。
 */
void BaseExporter::start() {
    if (running_) {
        log(2, "导出器已在运行");
        return;
    }
    
    // 初始化 lib-mq ReqRepQueue
    req_rep_queue_ = std::make_unique<libmq::ReqRepQueue>();
    
    libmq::MqConfig mqConfig;
    mqConfig.recvTimeoutMs = 2000;
    
    // 将日志回调传递到 lib-mq (实现日志链路统一)
    mqConfig.logCallback = [this](libmq::LogLevel level, const std::string& msg) {
        log(static_cast<int>(level), msg);
    };
    
    bool initOk = req_rep_queue_->initializeAsServer(config_.endpoint, mqConfig);
    if (!initOk) {
        log(3, "初始化 ReqRepQueue 失败: " + config_.endpoint);
        req_rep_queue_.reset();
        return;
    }
    
    log(1, "导出器已启动, 监听: " + config_.endpoint);
    
    // 启动后台线程
    running_ = true;
    server_thread_ = std::thread(&BaseExporter::runServerLoop, this);
}

/**
 * 停止导出器
 * 
 * 设置运行标志为 false,等待后台线程退出并清理资源。
 * 如果未运行,则直接返回。
 */
void BaseExporter::stop() {
    if (!running_) {
        return;
    }
    
    log(1, "正在停止导出器...");
    running_ = false;
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
    log(1, "导出器已停止");
}

/**
 * 手动处理一次请求 (阻塞模式)
 * 
 * 接收一个查询请求,构建 JSON 响应并返回。
 * 适用于不想使用后台线程的场景。
 */
void BaseExporter::handleOneRequest() {
    if (!req_rep_queue_) {
        return;
    }
    
    // 接收请求
    std::string request = req_rep_queue_->recvRequest(2000);
    
    if (request.empty()) {
        // 超时或错误: 没有收到请求,REP socket 仍在等待 recv 状态
        // 此时不能 send,直接返回继续下一次循环
        return;
    }
    
    // 成功收到请求,构建并发送响应
    // REP socket 状态: recv → send
    try {
        std::string response = buildResponse();
        auto result = req_rep_queue_->sendResponse(response);
        if (!result) {
            std::string error_msg = "发送响应失败: " + result.message + 
                                   " | 请求内容: " + request + 
                                   " | 响应长度: " + std::to_string(response.length());
            log(3, error_msg);
        }
    } catch (const std::exception& e) {
        // buildResponse() 异常时,发送错误响应以维持状态机
        log(3, "构建响应异常: " + std::string(e.what()));
        auto result = req_rep_queue_->sendResponse("{\"error\":\"internal error\"}");
        if (!result) {
            log(3, "发送错误响应失败: " + result.message);
        }
    }
}

/**
 * 后台线程主循环
 * 
 * 持续接收并处理监控查询请求,直到 running_ 为 false。
 * 每次循环调用 handleOneRequest() 处理一个请求。
 */
void BaseExporter::runServerLoop() {
    log(1, "服务端循环已启动");
    
    while (running_) {
        handleOneRequest();
    }
    
    log(1, "服务端循环已退出");
}

/**
 * 构建 JSON 响应
 * 
 * 采集通用进程指标和自定义业务指标,构建 JSON 响应字符串。
 * 
 * 响应格式:
 * {
 *   "process": "iot_live",
 *   "pid": 12345,
 *   "timestamp": 1716970800,
 *   "status": "running",
 *   "uptime_sec": 3600,
 *   "memory": { "rss_kb": 52480, "vms_kb": 156720, "shared_kb": 8192 },
 *   "cpu": { "percent": 2.5, "time_sec": 1250 },
 *   "threads": 8,
 *   "fd_count": 45,
 *   "custom_metric1": 123.0,  // 自定义指标
 *   "custom_metric2": 456.0
 * }
 * 
 * @return JSON 字符串
 */
std::string BaseExporter::buildResponse() {
    cJSON* root = cJSON_CreateObject();
    
    // 基本信息
    cJSON_AddStringToObject(root, "process", config_.process_name.c_str());
    cJSON_AddNumberToObject(root, "pid", getpid());
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(time(nullptr)));
    
    // 采集通用指标
    ProcessMetrics metrics = MetricsCollector::collectCurrent(config_.process_name);
    
    // 计算 CPU 使用率 (通过两次采样差值)
    if (last_cpu_collect_time_ > 0 && metrics.cpu_time_sec > last_cpu_time_) {
        time_t now = time(nullptr);
        double time_diff = difftime(now, last_cpu_collect_time_);
        if (time_diff > 0) {
            double cpu_diff = metrics.cpu_time_sec - last_cpu_time_;
            metrics.cpu_percent = (cpu_diff / time_diff) * 100.0;
            // 限制在合理范围 (0-100%)
            if (metrics.cpu_percent > 100.0) {
                metrics.cpu_percent = 100.0;
            }
        }
    }
    
    // 更新采样历史
    last_cpu_time_ = metrics.cpu_time_sec;
    last_cpu_collect_time_ = metrics.collect_time;
    
    // 添加通用指标
    cJSON_AddStringToObject(root, "status", metrics.status.c_str());
    cJSON_AddNumberToObject(root, "uptime_sec", static_cast<double>(metrics.uptime_sec));
    
    // 内存指标 (KB)
    cJSON* memory = cJSON_CreateObject();
    cJSON_AddNumberToObject(memory, "rss_kb", static_cast<double>(metrics.memory_rss_kb));
    cJSON_AddNumberToObject(memory, "vms_kb", static_cast<double>(metrics.memory_vms_kb));
    cJSON_AddNumberToObject(memory, "shared_kb", static_cast<double>(metrics.memory_shared_kb));
    cJSON_AddItemToObject(root, "memory", memory);
    
    // CPU 指标
    cJSON* cpu = cJSON_CreateObject();
    cJSON_AddNumberToObject(cpu, "percent", metrics.cpu_percent);
    cJSON_AddNumberToObject(cpu, "time_sec", static_cast<double>(metrics.cpu_time_sec));
    cJSON_AddItemToObject(root, "cpu", cpu);
    
    // 其他指标
    cJSON_AddNumberToObject(root, "threads", metrics.threads);
    cJSON_AddNumberToObject(root, "fd_count", metrics.fd_count);
    
    // 自定义指标 (遍历回调函数表)
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        for (auto it = custom_metrics_.begin(); it != custom_metrics_.end(); ++it) {
            const std::string& name = it->first;
            MetricCallback& callback = it->second;
            try {
                double value = callback();
                cJSON_AddNumberToObject(root, name.c_str(), value);
            } catch (const std::exception& e) {
                log(3, "采集指标 " + name + " 失败: " + e.what());
            }
        }
    }
    
    // 转换为 JSON 字符串
    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str ? json_str : "{}");
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return result;
}

/**
 * 日志输出
 * 
 * 优先使用用户自定义的 logCallback,如果未设置则使用默认输出 (cerr)。
 * 与 lib-mq 的日志机制保持一致。
 * 
 * @param level 日志级别 (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR)
 * @param msg 日志消息
 */
void BaseExporter::log(int level, const std::string& msg) {
    if (level < config_.log_level) {
        return;
    }
    
    // 如果用户设置了自定义日志回调,优先使用
    if (config_.logCallback) {
        config_.logCallback(static_cast<LogLevel>(level), msg);
        return;
    }
    
    // 默认日志输出 (输出到 cerr)
    const char* level_str = "";
    switch (level) {
        case 0: level_str = "DEBUG"; break;
        case 1: level_str = "INFO";  break;
        case 2: level_str = "WARN";  break;
        case 3: level_str = "ERROR"; break;
    }
    
    std::cerr << "[" << level_str << "] [exporter] " << msg << std::endl;
}

} // namespace exporter
