// Local control channel for the console: \\.\pipe\SideDroidHost.
// One JSON object per line; each request gets one response line. See docs/protocol.md.
#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace sd {

// Small helpers for the flat JSON objects this channel uses.
std::string jsonEscape(const std::string& s);
std::string jsonGetString(const std::string& json, const char* key);
long long jsonGetNumber(const std::string& json, const char* key, long long fallback);
bool jsonGetBool(const std::string& json, const char* key, bool fallback);

class IpcServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;
    // Serves `pipeName` (the part after \\.\pipe\) to several clients at once, so
    // that a busy pipe can never be mistaken for a host that is not running.
    IpcServer(std::wstring pipeName, Handler h) : pipeName_(std::move(pipeName)), handler_(std::move(h)) {}
    ~IpcServer() { stop(); }
    void start();
    void stop();

private:
    void worker(SECURITY_ATTRIBUTES* sa);
    void serveClient(HANDLE pipe, HANDLE ioEvent);
    bool waitIo(HANDLE pipe, OVERLAPPED& ov, DWORD* transferred);

    std::wstring pipeName_;
    Handler handler_;
    std::atomic<bool> running_{false};
    HANDLE stopEvent_ = nullptr;
    PSECURITY_DESCRIPTOR securityDescriptor_ = nullptr;
    SECURITY_ATTRIBUTES securityAttributes_{};
    std::vector<std::thread> workers_;
    static constexpr int kMaxClients = 4;
};

}  // namespace sd
