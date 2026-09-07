/**
 * metrics_collector.h
 * 通用进程指标采集器
 * 
 * 从 Linux /proc/[pid]/ 文件系统读取进程的通用指标:
 * - PID, 进程状态, 运行时间
 * - 内存使用 (RSS, VMS, Shared)
 * - CPU 占用率
 * - 线程数
 * - 打开的文件描述符数量
 * 
 * 特点:
 * - 无依赖: 只读取 /proc 文件系统,不需要额外库
 * - 高效: 直接解析内核接口,延迟 < 1ms
 * - 线程安全: 所有方法都是静态方法,无共享状态
 * 
 * 使用示例:
 *   // 示例 1: 采集当前进程指标
 *   ProcessMetrics metrics = MetricsCollector::collectCurrent("iot_live");
 *   printf("RSS: %ld KB\n", metrics.memory_rss_kb);
 *   printf("CPU: %.1f%%\n", metrics.cpu_percent);
 *   
 *   // 示例 2: 采集指定进程指标
 *   ProcessMetrics other = MetricsCollector::collect(1234, "ota_agent");
 *   printf("PID 1234 状态: %s\n", other.status.c_str());
 */

#ifndef LIBEXPORTER_METRICS_COLLECTOR_H
#define LIBEXPORTER_METRICS_COLLECTOR_H

#include <string>
#include <ctime>

namespace exporter {

/**
 * 进程指标数据结构体
 * 
 * 存储从 /proc 文件系统采集的进程各项指标。
 * 所有内存相关指标单位为 KB。
 */
struct ProcessMetrics {
    pid_t pid = 0;                      ///< 进程 ID
    std::string process_name;           ///< 进程名称 (从 /proc/[pid]/status 读取)
    std::string status;                 ///< 进程状态 (running, sleeping, stopped 等)
    
    // 内存指标 (单位: KB)
    long memory_rss_kb = 0;             ///< RSS: 实际使用的物理内存
    long memory_vms_kb = 0;             ///< VMS: 虚拟内存大小
    long memory_shared_kb = 0;          ///< Shared: 共享内存大小
    
    // CPU 指标
    double cpu_percent = 0.0;           ///< CPU 使用率百分比 (0-100)
    unsigned long cpu_time_sec = 0;     ///< 总 CPU 时间 (秒,包含用户态和内核态)
    
    // 其他指标
    int threads = 0;                    ///< 线程数
    int fd_count = 0;                   ///< 打开的文件描述符数量
    long uptime_sec = 0;                ///< 进程运行时间 (秒)
    
    // 时间戳
    time_t collect_time = 0;            ///< 指标采集时间戳
};

/**
 * 指标采集器类
 * 
 * 提供静态方法从 /proc 文件系统采集进程指标。
 * 所有方法都是线程安全的。
 */
class MetricsCollector {
public:
    /**
     * 采集指定进程的指标
     * 
     * @param pid 进程 ID
     * @param process_name 进程名称 (可选,如提供则不覆盖从 /proc 读取的名称)
     * @return ProcessMetrics 采集到的指标数据
     */
    static ProcessMetrics collect(pid_t pid, const std::string& process_name = "");
    
    /**
     * 采集当前进程的指标
     * 
     * @param process_name 进程名称 (可选)
     * @return ProcessMetrics 采集到的指标数据
     */
    static ProcessMetrics collectCurrent(const std::string& process_name = "");

private:
    // 从 /proc/[pid]/status 读取内存、线程等信息
    static void readProcStatus(pid_t pid, ProcessMetrics& metrics);
    
    // 从 /proc/[pid]/stat 读取 CPU 时间等信息
    static void readProcStat(pid_t pid, ProcessMetrics& metrics);
    
    // 计算 CPU 使用率 (需要两次采样)
    static double calculateCpuPercent(pid_t pid, unsigned long cpu_time, time_t collect_time);
    
    // 统计打开的文件描述符数量
    static int countOpenFds(pid_t pid);
    
    // 计算进程运行时间
    static long calculateUptime(pid_t pid);
    
    // 解析 /proc 文件中的数值
    static long parseProcValue(const std::string& line);
};

} // namespace exporter

#endif // LIBEXPORTER_METRICS_COLLECTOR_H
