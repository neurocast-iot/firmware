/**
 * @file ice_config.cpp
 * @brief 域名解析工具实现（自 test_p2p_pusher.cpp resolveHostIPv4 迁入）
 */
#include "rtc/ice_config.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace rtc {

bool resolveHostIPv4(const std::string& host, std::string& outIp) {
    struct addrinfo hints;
    struct addrinfo* res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        return false;
    }
    char buf[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in* sin = (struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);

    outIp = buf;
    return !outIp.empty();
}

} // namespace rtc
