// Keeps `adb reverse` set up for every authorized USB device, so that the phone
// app can reach the host on 127.0.0.1 with no configuration.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sd {

struct AdbDevice {
    std::string serial;
    std::string state;  // device, unauthorized, offline
};

// Parses `adb devices` output. Pure, for testing.
std::vector<AdbDevice> parseAdbDevices(const std::string& output);

class AdbWatcher {
public:
    explicit AdbWatcher(uint16_t port) : port_(port) {}
    ~AdbWatcher() { stop(); }
    void start();
    void stop();
    void killServer();
    bool adbFound() const { return found_.load(); }
    std::vector<AdbDevice> devices();

private:
    void loop();
    uint16_t port_;
    std::atomic<bool> running_{false}, found_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<AdbDevice> devices_;
    std::vector<std::string> reversed_;
};

}  // namespace sd
