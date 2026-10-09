#include "vrstream/udp_socket.h"

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
#endif

namespace vrstream {

namespace {

void initSocketsOnce() {
#ifdef _WIN32
    static bool done = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)done;
#endif
}

void setNonBlocking(int64_t s) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(static_cast<SocketHandle>(s), FIONBIO, &mode);
#else
    int flags = fcntl(static_cast<int>(s), F_GETFL, 0);
    fcntl(static_cast<int>(s), F_SETFL, flags | O_NONBLOCK);
#endif
}

std::string addrToString(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
    } else {
        const auto* a4 = reinterpret_cast<const sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof(buf));
    }
    return buf;
}

}  // namespace

UdpSocket::~UdpSocket() {
    if (sock_ != -1) {
#ifdef _WIN32
        closesocket(static_cast<SocketHandle>(sock_));
#else
        close(static_cast<int>(sock_));
#endif
    }
}

bool UdpSocket::bind(uint16_t port) {
    initSocketsOnce();
    sock_ = static_cast<int64_t>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (sock_ < 0) return false;

    int reuse = 1;
    setsockopt(static_cast<SocketHandle>(sock_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(static_cast<SocketHandle>(sock_), reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0) {
        return false;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    getsockname(static_cast<SocketHandle>(sock_), reinterpret_cast<sockaddr*>(&bound), &len);
    localPort_ = ntohs(bound.sin_port);
    setNonBlocking(sock_);
    int buf = 4 * 1024 * 1024;  // absorb bursts without dropping
    setsockopt(static_cast<SocketHandle>(sock_), SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&buf), sizeof(buf));
    return true;
}

bool UdpSocket::enableBroadcast() {
    int on = 1;
    return setsockopt(static_cast<SocketHandle>(sock_), SOL_SOCKET, SO_BROADCAST,
                      reinterpret_cast<const char*>(&on), sizeof(on)) == 0;
}

void UdpSocket::setDscpEf() {
#ifdef _WIN32
    // Windows silently ignores IP_TOS for unprivileged processes in many
    // configurations; QoS via SIO_APPLY_TRANSPORT_SETTING is not worth the
    // complexity for v1. WMM classification still favors small UDP.
    DWORD dscp = 46 << 2;
    setsockopt(static_cast<SocketHandle>(sock_), IPPROTO_IP, IP_TOS,
               reinterpret_cast<const char*>(&dscp), sizeof(dscp));
#else
    int tos = 46 << 2;
    setsockopt(static_cast<int>(sock_), IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#endif
}

bool UdpSocket::sendTo(const std::string& addr, uint16_t port, const uint8_t* data,
                       size_t len) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, addr.c_str(), &to.sin_addr) != 1) return false;
    int n = sendto(static_cast<SocketHandle>(sock_), reinterpret_cast<const char*>(data),
                   static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    return n == static_cast<int>(len);
}

size_t UdpSocket::recvFrom(uint8_t* buf, size_t cap, std::string& fromAddr,
                           uint16_t& fromPort) {
    sockaddr_storage from{};
    socklen_t flen = sizeof(from);
    int n = recvfrom(static_cast<SocketHandle>(sock_), reinterpret_cast<char*>(buf),
                     static_cast<int>(cap), 0, reinterpret_cast<sockaddr*>(&from), &flen);
    if (n <= 0) return 0;
    fromAddr = addrToString(from);
    if (from.ss_family == AF_INET6)
        fromPort = ntohs(reinterpret_cast<const sockaddr_in6*>(&from)->sin6_port);
    else
        fromPort = ntohs(reinterpret_cast<const sockaddr_in*>(&from)->sin_port);
    return static_cast<size_t>(n);
}

bool UdpSocket::waitReadable(uint64_t timeoutUs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(static_cast<SocketHandle>(sock_), &rfds);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutUs / 1000000);
    tv.tv_usec = static_cast<long>(timeoutUs % 1000000);
    int r = select(static_cast<int>(sock_) + 1, &rfds, nullptr, nullptr, &tv);
    return r > 0;
}

}  // namespace vrstream
