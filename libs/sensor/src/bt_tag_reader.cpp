/**
 * @file bt_tag_reader.cpp
 * @brief 蓝牙标签采集器实现
 *
 * 采集线程主循环：读串口 → 喂给帧解析器 → 解析出标签就回调 → sleep → 继续。
 * 串口用 select 超时读（SerialPort 内部实现），没数据时不会死等。
 */
#include "nc/sensor/bt_tag_reader.h"
#include "nc/common/log_utils.h"

#include <chrono>
#include <cstring>

namespace nc {
namespace sensor {

BtTagReader::BtTagReader(const std::string& device, int baudrate)
    : m_device(device)
    , m_baudrate(baudrate)
    , m_port(device, baudrate)
{
    NC_LOGI("[BtTagReader] 创建实例: device={}, baudrate={}", device.c_str(), baudrate);
}

BtTagReader::~BtTagReader() {
    stop();
}

void BtTagReader::setTagCallback(TagCallback callback) {
    m_callback = std::move(callback);
}

bool BtTagReader::start() {
    if (m_running.load()) {
        NC_LOGW("[BtTagReader] 已经在运行中，忽略重复启动");
        return true;
    }

    if (!m_port.open()) {
        NC_LOGE("[BtTagReader] 打开串口失败: {}", m_device.c_str());
        return false;
    }

    m_running.store(true);
    m_thread = std::thread(&BtTagReader::collectLoop, this);
    NC_LOGI("[BtTagReader] 采集线程已启动: {}", m_device.c_str());
    return true;
}

void BtTagReader::stop() {
    if (!m_running.load()) {
        return;  /* 本来就没开 */
    }

    m_running.store(false);

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_port.close();
    m_parser.reset();
    NC_LOGI("[BtTagReader] 采集线程已停止: {}", m_device.c_str());
}

bool BtTagReader::isRunning() const {
    return m_running.load();
}

/**
 * 采集线程主循环
 *
 * 每次循环：
 *   1. 从串口读一批字节（select 超时 100ms，没数据不卡住）
 *   2. 喂给帧解析器
 *   3. 循环调 parseNext() 把所有完整帧都解析出来
 *   4. 每解析出一个标签就通过回调通知上层
 *   5. 没数据或没帧了就 sleep 10ms 再试
 *
 * 为什么 sleep 10ms：
 *   串口波特率 9600，一个字节约 1ms，10ms 能攒够一帧数据，
 *   又不会让 CPU 空转。
 */
void BtTagReader::collectLoop() {
    uint8_t buf[256];

    while (m_running.load()) {
        /* 从串口读数据（超时 100ms，没数据返回 0） */
        int bytesRead = m_port.read(buf, sizeof(buf), 100);
        if (bytesRead > 0) {
            /* 喂给帧解析器 */
            m_parser.appendData(buf, bytesRead);

            /* 把所有完整帧都解析出来 */
            while (true) {
                BtTagInfo tag = m_parser.parseNext();
                if (tag.tagId.empty()) {
                    break;  /* 没有更多完整帧了 */
                }

                NC_LOGI("[BtTagReader] 解析到标签: id={} status=0x{:02X}",
                        tag.tagId.c_str(), tag.status);

                /* 通过回调通知上层 */
                if (m_callback) {
                    m_callback(tag);
                }
            }
        }

        if (bytesRead <= 0) {
            /* 没数据或超时，sleep 10ms 避免 CPU 空转 */
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

} // namespace sensor
} // namespace nc
