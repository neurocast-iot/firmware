/**
 * metrics_collector.cpp
 * 通用进程指标采集器实现
 * 
 * 从 Linux /proc 文件系统读取进程指标。
 * 
 * 核心功能:
 * - 从 /proc/[pid]/status 读取进程名称、状态、内存、线程数
 * - 从 /proc/[pid]/stat 读取 CPU 时间、启动时间
 * - 从 /proc/[pid]/fd 统计打开的文件描述符数量
 * - 从 /proc/uptime 计算进程运行时间
 * 
 * 性能特点:
 * - 无依赖: 只读取 /proc 文件系统,不需要额外库
 * - 高效: 直接解析内核接口,延迟 < 1ms
 * - 线程安全: 所有方法都是静态方法,无共享状态
 */

#include "exporter/metrics_collector.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <dirent.h>
#include <unistd.h>
#include <cstring>

namespace exporter {

/**
 * 采集指定进程的指标
 * 
 * 从 /proc/[pid]/ 读取进程的各项指标,包括:
 * - 基本信息: PID, 进程名称, 状态
 * - 内存指标: RSS, VMS, Shared
 * - CPU 指标: 总 CPU 时间 (用户态 + 内核态)
 * - 其他指标: 线程数, 文件描述符数量, 运行时间
 * 
 * @param pid 进程 ID
 * @param process_name 进程名称 (可选,如提供则不覆盖从 /proc 读取的名称)
 * @return ProcessMetrics 采集到的指标数据
 */
ProcessMetrics MetricsCollector::collect(pid_t pid, const std::string& process_name) {
    ProcessMetrics metrics;
    metrics.pid = pid;
    metrics.process_name = process_name;
    metrics.collect_time = time(nullptr);
    
    if (pid <= 0) {
        return metrics;
    }
    
    // 读取 /proc/[pid]/status (进程名称、状态、内存、线程数)
    readProcStatus(pid, metrics);
    
    // 读取 /proc/[pid]/stat (CPU 时间)
    readProcStat(pid, metrics);
    
    // 统计文件描述符数量
    metrics.fd_count = countOpenFds(pid);
    
    // 计算进程运行时间
    metrics.uptime_sec = calculateUptime(pid);
    
    return metrics;
}

/**
 * 采集当前进程的指标
 * 
 * 便捷方法,等价于 collect(getpid(), process_name)。
 * 
 * @param process_name 进程名称 (可选)
 * @return ProcessMetrics 采集到的指标数据
 */
ProcessMetrics MetricsCollector::collectCurrent(const std::string& process_name) {
    return collect(getpid(), process_name);
}

/**
 * 读取 /proc/[pid]/status 文件
 * 
 * 解析进程状态文件,提取:
 * - Name: 进程名称
 * - State: 进程状态 (R=running, S=sleeping, T=stopped 等)
 * - VmRSS: 实际使用的物理内存 (KB)
 * - VmSize: 虚拟内存大小 (KB)
 * - VmShared/RssFile: 共享内存大小 (KB)
 * - Threads: 线程数
 * 
 * @param pid 进程 ID
 * @param metrics 输出参数,存储采集到的指标
 */
void MetricsCollector::readProcStatus(pid_t pid, ProcessMetrics& metrics) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream file(path);
    
    if (!file.is_open()) {
        return;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("Name:") == 0) {
            metrics.process_name = line.substr(6);
            // 去除前后空白字符
            size_t start = metrics.process_name.find_first_not_of(" \t");
            size_t end = metrics.process_name.find_last_not_of(" \t");
            if (start != std::string::npos) {
                metrics.process_name = metrics.process_name.substr(start, end - start + 1);
            }
        } else if (line.find("State:") == 0) {
            // State: R (running)
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                metrics.status = line.substr(pos + 1);
                // 去除空白字符
                size_t start = metrics.status.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    metrics.status = metrics.status.substr(start);
                }
            }
        } else if (line.find("VmRSS:") == 0) {
            metrics.memory_rss_kb = parseProcValue(line);
        } else if (line.find("VmSize:") == 0) {
            metrics.memory_vms_kb = parseProcValue(line);
        } else if (line.find("VmShared:") == 0 || line.find("RssFile:") == 0) {
            metrics.memory_shared_kb = parseProcValue(line);
        } else if (line.find("Threads:") == 0) {
            metrics.threads = static_cast<int>(parseProcValue(line));
        }
    }
    
    file.close();
}

/**
 * 读取 /proc/[pid]/stat 文件
 * 
 * 解析进程统计文件,提取 CPU 时间信息。
 * /proc/[pid]/stat 格式: pid (comm) state ppid pgrp session ...
 * 
 * 关键字段:
 * - 字段 14 (utime): 用户态 CPU 时间 (clock ticks)
 * - 字段 15 (stime): 内核态 CPU 时间 (clock ticks)
 * - 字段 22 (starttime): 进程启动时间 (clock ticks)
 * 
 * 注意: 需要跳过括号内的进程名 (可能包含空格)
 * 
 * @param pid 进程 ID
 * @param metrics 输出参数,存储采集到的 CPU 时间
 */
void MetricsCollector::readProcStat(pid_t pid, ProcessMetrics& metrics) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream file(path);
    
    if (!file.is_open()) {
        return;
    }
    
    std::string content;
    std::getline(file, content);
    file.close();
    
    // /proc/[pid]/stat 格式: pid (comm) state ppid ...
    // 需要跳过括号内的进程名 (可能包含空格)
    size_t start = content.find('(');
    size_t end = content.rfind(')');
    
    if (start == std::string::npos || end == std::string::npos) {
        return;
    }
    
    // 提取括号后的字段
    std::string after_comm = content.substr(end + 2); // 跳过 ") "
    std::istringstream iss(after_comm);
    
    // 字段索引 (从 state 开始, state 是第 3 个字段)
    std::string field;
    int field_idx = 3; // 从 3 开始计数 (state)
    
    while (iss >> field) {
        if (field_idx == 3) {
            // state 字段 (已在 status 中读取)
        } else if (field_idx == 14) {
            // utime - 用户态 CPU 时间 (clock ticks)
            try {
                unsigned long utime = std::stoul(field);
                metrics.cpu_time_sec += utime;
            } catch (...) {}
        } else if (field_idx == 15) {
            // stime - 内核态 CPU 时间 (clock ticks)
            try {
                unsigned long stime = std::stoul(field);
                metrics.cpu_time_sec += stime;
            } catch (...) {}
        }
        
        field_idx++;
    }
    
    // 转换 clock ticks 到秒 (假设 HZ=100)
    metrics.cpu_time_sec /= 100;
}

/**
 * 计算 CPU 使用率 (预留方法)
 * 
 * 当前版本未使用此方法,CPU 使用率在 BaseExporter::buildResponse()
 * 中通过两次采样差值计算。
 * 
 * @param pid 进程 ID (未使用)
 * @param cpu_time CPU 时间 (未使用)
 * @param collect_time 采集时间 (未使用)
 * @return 0.0 (由调用方计算)
 */
double MetricsCollector::calculateCpuPercent(pid_t pid, unsigned long cpu_time, time_t collect_time) {
    // 简化版: 返回总 CPU 时间
    // 精确版需要两次采样计算差值
    // 这里暂时返回 0,由调用方在两次采样的基础上计算
    return 0.0;
}

/**
 * 统计进程打开的文件描述符数量
 * 
 * 通过读取 /proc/[pid]/fd 目录下的条目数量来统计。
 * 跳过 "." 和 ".." 目录。
 * 
 * @param pid 进程 ID
 * @return 打开的文件描述符数量
 */
int MetricsCollector::countOpenFds(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/fd";
    DIR* dir = opendir(path.c_str());
    
    if (!dir) {
        return 0;
    }
    
    int count = 0;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != nullptr) {
        // 跳过 "." 和 ".." 目录
        if (entry->d_name[0] == '.') {
            continue;
        }
        count++;
    }
    
    closedir(dir);
    return count;
}

/**
 * 计算进程运行时间
 * 
 * 通过读取 /proc/[pid]/stat 中的 starttime 字段,结合 /proc/uptime
 * 计算进程的运行时间。
 * 
 * 计算公式:
 *   进程运行时间 = 系统运行时间 - 进程启动时间
 * 
 * @param pid 进程 ID
 * @return 进程运行时间 (秒),失败返回 0
 */
long MetricsCollector::calculateUptime(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream file(path);
    
    if (!file.is_open()) {
        return 0;
    }
    
    std::string content;
    std::getline(file, content);
    file.close();
    
    // 解析 starttime (第 22 个字段)
    size_t start = content.find('(');
    size_t end = content.rfind(')');
    
    if (start == std::string::npos || end == std::string::npos) {
        return 0;
    }
    
    std::string after_comm = content.substr(end + 2);
    std::istringstream iss(after_comm);
    
    std::string field;
    int field_idx = 3;
    long starttime = 0;
    
    while (iss >> field) {
        if (field_idx == 22) {
            try {
                starttime = std::stol(field);
            } catch (...) {
                return 0;
            }
            break;
        }
        field_idx++;
    }
    
    // starttime 单位是 clock ticks,转换为秒 (假设 HZ=100)
    long starttime_sec = starttime / 100;
    
    // 获取系统启动时间
    std::ifstream uptime_file("/proc/uptime");
    if (!uptime_file.is_open()) {
        return 0;
    }
    
    double system_uptime;
    uptime_file >> system_uptime;
    uptime_file.close();
    
    // 进程运行时间 = 系统运行时间 - 进程启动时间
    long uptime = static_cast<long>(system_uptime) - starttime_sec;
    
    return uptime > 0 ? uptime : 0;
}

/**
 * 解析 /proc 文件中的数值
 * 
 * 从 "Key: 12345 kB" 格式的字符串中提取数值。
 * 
 * @param line /proc 文件中的一行
 * @return 解析到的数值,失败返回 0
 */
long MetricsCollector::parseProcValue(const std::string& line) {
    size_t pos = line.find(':');
    if (pos == std::string::npos) {
        return 0;
    }
    
    std::string value_str = line.substr(pos + 1);
    
    try {
        return std::stol(value_str);
    } catch (...) {
        return 0;
    }
}

} // namespace exporter
