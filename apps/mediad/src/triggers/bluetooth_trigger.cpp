/**
 * @file bluetooth_trigger.cpp
 * @brief 蓝牙标签触发源实现
 *
 * 采集线程（BtTagReader）发现标签 → onTagDetected 加入缓存 → 唤醒拍照线程。
 * 拍照线程循环：缓存不空就拍一张 → 复制给所有标签 → count++ → 满数踢出。
 * 一张照片服务所有标签，缓存空了才停。
 */
#include "triggers/bluetooth_trigger.h"

#include "nc/common/file_utils.h"
#include "nc/common/log_utils.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace mediad {

BluetoothTrigger::BluetoothTrigger(const Config& cfg)
    : m_cfg(cfg)
    , m_id(cfg.id)
{
}

BluetoothTrigger::~BluetoothTrigger() {
    disable();
}

void BluetoothTrigger::enable() {
    if (m_enabled.load()) {
        return;  /* 已在跑，不重复创建 */
    }

    /* 创建 BtTagReader，打开串口，启动采集线程 */
    m_reader.reset(new nc::sensor::BtTagReader(BT_SERIAL_PORT, BT_BAUDRATE));
    m_reader->setTagCallback([this](const nc::sensor::BtTagInfo& tag) {
        onTagDetected(tag);
    });

    if (!m_reader->start()) {
        NC_LOGE("BluetoothTrigger[{}]: 串口打开失败: {}",
                m_id.c_str(), BT_SERIAL_PORT);
        m_reader.reset();
        return;
    }

    m_enabled.store(true);
    NC_LOGI("BluetoothTrigger[{}]: enabled (port={}, baudrate={})",
            m_id.c_str(), BT_SERIAL_PORT, BT_BAUDRATE);
}

void BluetoothTrigger::disable() {
    if (!m_enabled.load()) {
        return;  /* 本来就没开 */
    }

    m_enabled.store(false);

    /* 停拍照线程（如果还在跑） */
    m_running.store(false);
    m_photoCv.notify_all();
    if (m_photoThread.joinable()) {
        m_photoThread.join();
    }

    /* 停采集线程 */
    if (m_reader) {
        m_reader->stop();
        m_reader.reset();
    }

    NC_LOGI("BluetoothTrigger[{}]: disabled", m_id.c_str());
}

bool BluetoothTrigger::isEnabled() const {
    return m_enabled.load();
}

/**
 * 标签检测回调（在采集线程上下文执行）
 *
 * 只做两件事：把标签加入缓存、唤醒拍照线程。
 * 不阻塞采集线程，拍照由拍照线程异步完成。
 * 已经在缓存里的标签（正在拍）自动忽略，不会重复加入。
 */
void BluetoothTrigger::onTagDetected(const nc::sensor::BtTagInfo& tag) {
    bool needStartThread = false;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_tagCache.count(tag.tagId) > 0) {
            return;  /* 这个标签正在拍，忽略 */
        }
        m_tagCache[tag.tagId] = {0, m_cfg.burstCount};
        
        /* 判断是否需要启动拍照线程：
         * - 线程不在跑（m_running=false）→ 需要启动新线程
         * - 线程在跑（m_running=true）→ 只需唤醒它 */
        if (!m_running.load()) {
            /* 回收已结束的线程（如果有），避免线程对象泄漏 */
            if (m_photoThread.joinable()) {
                m_photoThread.join();
            }
            needStartThread = true;
        }
        
        NC_LOGI("BluetoothTrigger[{}]: 标签加入缓存 id={} 当前缓存数={}",
                m_id.c_str(), tag.tagId.c_str(), static_cast<int>(m_tagCache.size()));
    }
    if (needStartThread) {
        m_running.store(true);
        m_photoThread = std::thread(&BluetoothTrigger::photoLoop, this);
    } else {
        /* 拍照线程已在跑，唤醒它处理新标签 */
        m_photoCv.notify_one();
    }
}

/**
 * 拍照线程主循环
 *
 * 由 onTagDetected 在第一个标签进来时启动。
 * 缓存不空就循环：拍一张 → 复制给所有标签 → count++ → 踢满数的。
 * 一张照片服务所有标签，减少拍照次数。
 * 缓存空了线程就退出，等下一个标签来再重新启动。
 */
void BluetoothTrigger::photoLoop() {
    NC_LOGI("BluetoothTrigger[{}]: photo loop started", m_id.c_str());

    while (m_running.load()) {
        {
            std::unique_lock<std::mutex> lock(m_cacheMutex);
            m_photoCv.wait(lock, [this] {
                return !m_running.load() || !m_tagCache.empty();
            });
            if (!m_running.load()) {
                break;
            }
        }

        /* 构建事件，拍一张照片 */
        TriggerEvent event = makeTriggerEvent(
            ActionType::Snapshot, m_cfg.priority,
            m_id.c_str(), "bluetooth_");
        event.burstCount = 1;  /* 每次只拍一张，连拍由这个循环控制 */

        SnapshotResult result;
        if (onTriggered) {
            result = onTriggered(event);
        }

        if (!result.ok) {
            /* 拍照失败（相机未就绪/被抢占/存储满等），跳过这轮，
             * 标签留在缓存里，下一轮再试 */
            NC_LOGW("BluetoothTrigger[{}]: snapshot failed, will retry", m_id.c_str());
            if (m_cfg.burstIntervalMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(m_cfg.burstIntervalMs));
            }
            continue;
        }

        NC_LOGI("BluetoothTrigger[{}]: photo taken: {}", m_id.c_str(), result.path.c_str());

        /* 复制给每个标签，更新计数 */
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            /* 复制给每个标签，计数++，满数的直接踢出 */
            for (auto it = m_tagCache.begin(); it != m_tagCache.end(); ) {
                std::string copiedPath = copyPhotoForTag(result.path, it->first);
                if (!copiedPath.empty()) {
                    it->second.count++;
                    NC_LOGI("BluetoothTrigger[{}]: tag {} count={}/{} -> {}",
                            m_id.c_str(), it->first.c_str(),
                            it->second.count, it->second.burstCount,
                            copiedPath.c_str());
                    /* 通知上层：这个标签的副本已生成（副本大小跟源文件一样）
                     * 蓝牙触发源不生成缩略图，传空 */
                    if (m_fileReadyCallback) {
                        m_fileReadyCallback("bluetooth", copiedPath, result.size,
                                           "", 0);
                    }
                }
                if (it->second.count >= it->second.burstCount) {
                    NC_LOGI("BluetoothTrigger[{}]: tag {} 拍满 {} 张，移除",
                            m_id.c_str(), it->first.c_str(), it->second.burstCount);
                    it = m_tagCache.erase(it);
                } else {
                    ++it;
                }
            }

            /* 缓存空了：拍照线程退出，等下一个标签来再启动 */
            if (m_tagCache.empty()) {
                m_running.store(false);
                break;
            }
        }

        /* 连拍间隔 */
        if (m_cfg.burstIntervalMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_cfg.burstIntervalMs));
        }
    }

    NC_LOGI("BluetoothTrigger[{}]: photo loop stopped", m_id.c_str());
}

/**
 * 把照片文件复制一份给指定标签
 *
 * 从拍照生成的源文件复制一份，文件名加上 tagId 区分。
 * 输出目录跟源文件同级（从源路径推导）。
 *
 * @return 复制后的文件路径，失败返回空串
 */
std::string BluetoothTrigger::copyPhotoForTag(const std::string& srcPath, const std::string& tagId) {
    /* 从源路径提取目录和文件名：
     * srcPath = /mnt/emmc/photos/20260811/23/bluetooth_20260811_230843132.jpg
     * dir     = /mnt/emmc/photos/20260811/23/
     * base    = bluetooth_20260811_230843132.jpg */
    auto lastSlash = srcPath.rfind('/');
    if (lastSlash == std::string::npos) {
        return "";
    }
    std::string dir = srcPath.substr(0, lastSlash + 1);
    std::string base = srcPath.substr(lastSlash + 1);

    /* 插入 tagId：bluetooth_20260811_... → bluetooth_{tagId}_20260811_... */
    auto firstUnderscore = base.find('_');
    std::string dstName;
    if (firstUnderscore != std::string::npos) {
        dstName = base.substr(0, firstUnderscore + 1)  /* "bluetooth_" */
                + tagId + "_"
                + base.substr(firstUnderscore + 1);     /* "20260811_..." */
    } else {
        dstName = tagId + "_" + base;
    }

    std::string dstPath = dir + dstName;

    if (!nc::common::CopyFile(srcPath, dstPath)) {
        NC_LOGW("BluetoothTrigger: 复制文件失败: {} -> {}", srcPath.c_str(), dstPath.c_str());
        return "";
    }

    return dstPath;
}

TriggerCfg BluetoothTrigger::getConfig() const {
    TriggerCfg cfg;
    cfg.id = m_id;
    cfg.type = "bluetooth";
    cfg.enabled = m_enabled.load();
    cfg.priority = m_cfg.priority;
    cfg.burstCount = m_cfg.burstCount;
    cfg.burstIntervalMs = m_cfg.burstIntervalMs;
    /* 蓝牙触发源没有 schedule 字段，留空 */
    return cfg;
}

} // namespace mediad
