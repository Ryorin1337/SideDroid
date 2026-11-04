// Thin cross-platform socket wrapper (Winsock / BSD).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace sd {

uint64_t monotonicUs();
void netInit();

class Socket {
public:
    Socket() = default;
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = kInvalid; }
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) { close(); fd_ = o.fd_; o.fd_ = kInvalid; }
        return *this;
    }

    static Socket tcpConnect(const std::string& host, uint16_t port, int timeoutMs);
    static Socket tcpListen(uint16_t port, bool loopbackOnly);
    static Socket udpBind(uint16_t port);

    bool valid() const { return fd_ != kInvalid; }
    void close();
    // Stops blocked calls on other threads, without releasing the descriptor.
    void shutdownBoth();

    // Returns an invalid socket on timeout or error.
    Socket accept(int timeoutMs, std::string* peerIp, bool* isLoopback);

    bool sendAll(const void* p, size_t n);
    bool recvAll(void* p, size_t n);

    void setNoDelay();
    void setBuffers(int sendBytes, int recvBytes);
    void setRecvTimeout(int ms);

    // UDP. recvFrom returns bytes received, 0 on timeout, -1 on error.
    int recvFrom(void* p, size_t n, uint32_t* ip, uint16_t* port);
    bool sendTo(const void* p, size_t n, uint32_t ip, uint16_t port);
    uint16_t localPort() const;
    // IPv4 of the connected peer in network byte order, or 0.
    uint32_t peerIpv4() const;

private:
#if defined(_WIN64)
    using Fd = unsigned long long;
#elif defined(_WIN32)
    using Fd = unsigned int;
#else
    using Fd = int;
#endif
    static constexpr Fd kInvalid = static_cast<Fd>(-1);
    explicit Socket(Fd f) : fd_(f) {}
    Fd fd_ = kInvalid;
};

}  // namespace sd
