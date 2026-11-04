// Local control channel for the console: \\.\pipe\SideDroidHost.
// One JSON object per line; each request gets one response line. See docs/protocol.md.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace sd {

// Small helpers for the flat JSON objects this channel uses.
std::string jsonEscape(const std::string& s);
std::string jsonGetString(const std::string& json, const char* key);
long long jsonGetNumber(const std::string& json, const char* key, long long fallback);
bool jsonGetBool(const std::string& json, const char* key, bool fallback);

class IpcServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;
    explicit IpcServer(Handler h) : handler_(std::move(h)) {}
    ~IpcServer() { stop(); }
    void start();
    void stop();

private:
    void loop();
    Handler handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace sd
