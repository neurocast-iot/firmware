/**
 * @file serial_port.cpp
 * @brief 串口底层工具类实现
 *
 * 从 gw_av100 的 ZigbeeReader 里把串口相关的代码抽出来的。
 * 打开、配置波特率、读写字节、select 超时 —— 这些和具体协议无关，
 * 所有串口传感器都用同一份代码。
 */
#include "nc/sensor/serial_port.h"
#include "nc/common/log_utils.h"

#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>

namespace nc {
namespace sensor {

/* ====================================================================
 * 构造 / 析构
 * ==================================================================== */
SerialPort::SerialPort(const std::string& device, int baudrate)
    : m_device(device)
    , m_baudrate(baudrate)
    , m_fd(-1)
    , m_isOpen(false)
    , m_oldTermios(nullptr)
{
}

SerialPort::~SerialPort() {
    close();
}

/* ====================================================================
 * open：打开串口设备
 *
 * O_RDWR    = 可读可写
 * O_NOCTTY  = 不要把这个串口当成控制终端（不然 Ctrl+C 之类的信号会干扰）
 * O_NDELAY  = 非阻塞模式（read 没数据时不卡住，立刻返回）
 * ==================================================================== */
bool SerialPort::open() {
    if (m_isOpen) {
        NC_LOGW("[SerialPort] 串口已经打开: {}", m_device.c_str());
        return true;
    }

    m_fd = ::open(m_device.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    if (m_fd < 0) {
        NC_LOGE("[SerialPort] 打开串口失败: {}, 错误: {}", m_device.c_str(), strerror(errno));
        return false;
    }

    NC_LOGI("[SerialPort] 打开串口成功: {} (fd={})", m_device.c_str(), m_fd);

    if (!configure()) {
        NC_LOGE("[SerialPort] 配置串口失败: {}", m_device.c_str());
        ::close(m_fd);
        m_fd = -1;
        return false;
    }

    m_isOpen = true;
    return true;
}

/* ====================================================================
 * close：关闭串口，恢复原来的终端配置
 * ==================================================================== */
void SerialPort::close() {
    if (!m_isOpen || m_fd < 0) {
        return;
    }

    /* 恢复打开前的终端配置（不然关了之后串口参数还留着我们改过的值） */
    if (m_oldTermios) {
        tcsetattr(m_fd, TCSANOW, m_oldTermios);
        delete m_oldTermios;
        m_oldTermios = nullptr;
    }

    ::close(m_fd);
    m_fd = -1;
    m_isOpen = false;
    NC_LOGI("[SerialPort] 串口已关闭: {}", m_device.c_str());
}

bool SerialPort::isOpen() const {
    return m_isOpen && (m_fd >= 0);
}

/* ====================================================================
 * configure：配置串口参数
 *
 * 8N1 模式：8 数据位、无校验、1 停止位（最常用的串口配置）
 * 原始模式：不做任何行处理（不是按"行"读，而是来一个字节收一个字节）
 * ==================================================================== */
bool SerialPort::configure() {
    if (m_fd < 0) return false;

    /* 保存原来的终端配置，关闭时恢复 */
    m_oldTermios = new termios();
    if (tcgetattr(m_fd, m_oldTermios) != 0) {
        NC_LOGE("[SerialPort] 获取终端配置失败: {}", strerror(errno));
        return false;
    }

    struct termios newTermios;
    memcpy(&newTermios, m_oldTermios, sizeof(struct termios));

    /* 设置波特率 */
    speed_t baud;
    switch (m_baudrate) {
        case 9600:   baud = B9600;   break;
        case 19200:  baud = B19200;  break;
        case 38400:  baud = B38400;  break;
        case 57600:  baud = B57600;  break;
        case 115200: baud = B115200; break;
        case 230400: baud = B230400; break;
        default:
            NC_LOGE("[SerialPort] 不支持的波特率: {}", m_baudrate);
            return false;
    }
    cfsetispeed(&newTermios, baud);
    cfsetospeed(&newTermios, baud);

    /* 8N1：8 数据位、无校验、1 停止位 */
    newTermios.c_cflag &= ~PARENB;   /* 无校验 */
    newTermios.c_cflag &= ~CSTOPB;   /* 1 停止位 */
    newTermios.c_cflag &= ~CSIZE;    /* 清除数据位掩码 */
    newTermios.c_cflag |= CS8;       /* 8 数据位 */
    newTermios.c_cflag &= ~CRTSCTS;  /* 禁用硬件流控制 */
    newTermios.c_cflag |= (CLOCAL | CREAD);  /* 启用接收器 */

    /* 原始模式（不做行处理，来一个字节收一个字节） */
    newTermios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    newTermios.c_iflag &= ~(IXON | IXOFF | IXANY);  /* 禁用软件流控制 */
    newTermios.c_oflag &= ~OPOST;   /* 禁用输出处理 */

    /* 非阻塞读取：没数据时 read 立刻返回，不等 */
    newTermios.c_cc[VMIN] = 0;
    newTermios.c_cc[VTIME] = 1;  /* 0.1 秒超时（内核级别的小超时，防止 CPU 空转） */

    if (tcsetattr(m_fd, TCSANOW, &newTermios) != 0) {
        NC_LOGE("[SerialPort] 设置终端配置失败: {}", strerror(errno));
        return false;
    }

    /* 清空缓冲区（把打开前残留的脏数据清掉） */
    tcflush(m_fd, TCIOFLUSH);

    NC_LOGI("[SerialPort] 串口配置完成: {}, 8N1", m_device.c_str());
    return true;
}

/* ====================================================================
 * write：发送数据
 * ==================================================================== */
int SerialPort::write(const uint8_t* data, int len) {
    if (!isOpen() || !data || len <= 0) return -1;

    ssize_t written = ::write(m_fd, data, len);
    if (written < 0) {
        NC_LOGE("[SerialPort] 写入失败: {}", strerror(errno));
        return -1;
    }
    return static_cast<int>(written);
}

/* ====================================================================
 * read：接收数据（带超时）
 *
 * 用 select 等数据到来：
 *   - 有数据 → 读到 outBuffer 里，返回读到的字节数
 *   - 超时   → 返回 0
 *   - 出错   → 返回负数
 * ==================================================================== */
int SerialPort::read(uint8_t* outBuffer, int maxLen, int timeoutMs) {
    if (!isOpen() || !outBuffer || maxLen <= 0) return -1;

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(m_fd, &readSet);

    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int selResult = select(m_fd + 1, &readSet, nullptr, nullptr, &tv);
    if (selResult < 0) {
        NC_LOGE("[SerialPort] select 错误: {}", strerror(errno));
        return -1;
    }
    if (selResult == 0) {
        return 0;  /* 超时 */
    }

    ssize_t bytesRead = ::read(m_fd, outBuffer, maxLen);
    if (bytesRead < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 非阻塞模式下没数据，不算错误 */
        }
        NC_LOGE("[SerialPort] 读取错误: {}", strerror(errno));
        return -1;
    }

    return static_cast<int>(bytesRead);
}

} // namespace sensor
} // namespace nc
