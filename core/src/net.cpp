#include "sidedroid/net.h"

#include <chrono>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define SD_CLOSE closesocket
#define SD_SHUT SD_BOTH
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#define SD_CLOSE ::close
#define SD_SHUT SHUT_RDWR
#endif

namespace sd {

uint64_t monotonicUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void netInit() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
#endif
}

static bool waitReadable(unsigned long long fd, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD p{static_cast<SOCKET>(fd), POLLRDNORM, 0};
    return WSAPoll(&p, 1, timeoutMs) > 0;
#else
    pollfd p{static_cast<int>(fd), POLLIN, 0};
    return ::poll(&p, 1, timeoutMs) > 0;
#endif
}

void Socket::close() {
    if (valid()) {
        SD_CLOSE(fd_);
        fd_ = kInvalid;
    }
}

void Socket::shutdownBoth() {
    if (valid()) ::shutdown(fd_, SD_SHUT);
}

Socket Socket::tcpConnect(const std::string& host, uint16_t port, int timeoutMs) {
    netInit();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return {};
    Fd fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == kInvalid) {
        freeaddrinfo(res);
        return {};
    }
    Socket s(fd);

    // Non-blocking connect so that the timeout is honored.
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    int rc = ::connect(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc != 0) {
#ifdef _WIN32
        WSAPOLLFD p{fd, POLLWRNORM, 0};
        if (WSAPoll(&p, 1, timeoutMs) <= 0) return {};
#else
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, timeoutMs) <= 0) return {};
#endif
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) return {};
    }
#ifdef _WIN32
    nb = 0;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    fcntl(fd, F_SETFL, flags);
#endif
    s.setNoDelay();
    return s;
}

Socket Socket::tcpListen(uint16_t port, bool loopbackOnly) {
    netInit();
    Fd fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalid) return {};
    Socket s(fd);
#ifndef _WIN32
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return {};
    if (::listen(fd, 8) != 0) return {};
    return s;
}

Socket Socket::udpBind(uint16_t port) {
    netInit();
    Fd fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalid) return {};
    Socket s(fd);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return {};
    return s;
}

Socket Socket::accept(int timeoutMs, std::string* peerIp, bool* isLoopback) {
    if (!waitReadable(fd_, timeoutMs)) return {};
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    Fd c = ::accept(fd_, reinterpret_cast<sockaddr*>(&a), &len);
    if (c == kInvalid) return {};
    if (peerIp) {
        char buf[64]{};
        inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
        *peerIp = buf;
    }
    if (isLoopback) *isLoopback = (ntohl(a.sin_addr.s_addr) >> 24) == 127;
    Socket s(c);
    s.setNoDelay();
    return s;
}

bool Socket::sendAll(const void* p, size_t n) {
    auto b = static_cast<const char*>(p);
    while (n > 0) {
#ifdef _WIN32
        int w = ::send(fd_, b, static_cast<int>(n > 0x100000 ? 0x100000 : n), 0);
#else
        auto w = ::send(fd_, b, n, MSG_NOSIGNAL);
#endif
        if (w <= 0) return false;
        b += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

bool Socket::recvAll(void* p, size_t n) {
    auto b = static_cast<char*>(p);
    while (n > 0) {
#ifdef _WIN32
        int r = ::recv(fd_, b, static_cast<int>(n > 0x100000 ? 0x100000 : n), 0);
#else
        auto r = ::recv(fd_, b, n, 0);
#endif
        if (r <= 0) return false;
        b += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

void Socket::setNoDelay() {
    int yes = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
}

void Socket::setBuffers(int sendBytes, int recvBytes) {
    if (sendBytes > 0)
        setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBytes), sizeof(int));
    if (recvBytes > 0)
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBytes), sizeof(int));
}

void Socket::setRecvTimeout(int ms) {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval t{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
}

int Socket::recvFrom(void* p, size_t n, uint32_t* ip, uint16_t* port) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    auto r = ::recvfrom(fd_, static_cast<char*>(p), static_cast<int>(n), 0,
                        reinterpret_cast<sockaddr*>(&a), &len);
    if (r < 0) {
#ifdef _WIN32
        int e = WSAGetLastError();
        // WSAECONNRESET is an ICMP port-unreachable from an earlier send, not a socket failure.
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK || e == WSAECONNRESET || e == WSAEMSGSIZE)
            return 0;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
#endif
        return -1;
    }
    if (ip) *ip = a.sin_addr.s_addr;
    if (port) *port = ntohs(a.sin_port);
    return static_cast<int>(r);
}

bool Socket::sendTo(const void* p, size_t n, uint32_t ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    auto w = ::sendto(fd_, static_cast<const char*>(p), static_cast<int>(n), 0,
                      reinterpret_cast<sockaddr*>(&a), sizeof(a));
    return w == static_cast<decltype(w)>(n);
}

uint16_t Socket::localPort() const {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

uint32_t Socket::peerIpv4() const {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (getpeername(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return a.sin_addr.s_addr;
}

}  // namespace sd
