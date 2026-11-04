// The host application: accepts one phone at a time and runs the video pipeline.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "adb.h"
#include "discovery.h"
#include "input.h"
#include "ipc.h"
#include "sidedroid/fec.h"
#include "sidedroid/ratecontrol.h"
#include "sidedroid/secure.h"
#include "store.h"

namespace sd {

struct HostOptions {
    uint16_t port = kDefaultPort;
    // Capture this output instead of the virtual display, without changing any
    // display settings. For testing and for machines without the driver.
    std::wstring sourceOverride;
    bool enableAdb = true;
    bool enableIpc = true;
    bool enableDiscovery = true;
    bool enableTouch = true;
    bool testPattern = false;  // overlay moving noise on the capture, for bitrate tests
};

class Session;

class Host {
public:
    explicit Host(HostOptions opts);
    ~Host();
    bool start();
    void stop();
    std::string handleCommand(const std::string& json);
    Store& store() { return store_; }
    const HostOptions& options() const { return opts_; }

    // For Session.
    void publishStatus(const std::function<void(struct LiveStatus&)>& edit);
    void markOutputAttached(const std::wstring& device);
    Settings currentSettings() { return store_.settings(); }
    // True when SideDroid installed the driver, so the display is ours to manage.
    bool ownsDriver() const { return ownsDriver_; }

private:
    void acceptLoop();
    std::string statusJson();
    void detachIfIdle(bool force);
    std::optional<std::string> takePairingCodeAttempt();

    HostOptions opts_;
    Store store_;
    AdbWatcher adb_;
    Discovery discovery_;
    std::unique_ptr<IpcServer> ipc_;
    Socket listener_;
    std::atomic<bool> running_{false};
    bool ownsDriver_ = false;
    std::thread acceptThread_;

    std::mutex sessionMutex_;
    std::shared_ptr<Session> session_;
    std::thread sessionThread_;
    std::atomic<bool> sessionActive_{false};

    std::mutex pairMutex_;
    std::string pairingCode_;
    uint64_t pairingExpiresUs_ = 0;
    int pairingAttempts_ = 0;
    std::string lastPairedName_;

    std::mutex statusMutex_;
    std::unique_ptr<struct LiveStatus> live_;

    // Virtual display attached by us and waiting to be taken off the desktop.
    std::mutex attachMutex_;
    std::wstring attachedOutput_;
    uint64_t detachAtUs_ = 0;
};

struct LiveStatus {
    bool connected = false;
    std::string device, transport, encoder, codec, adapter, error;
    int displayW = 0, displayH = 0, hz = 0, encodeW = 0, encodeH = 0, fps = 0;
    uint32_t kbps = 0, measuredKbps = 0, rttMs = 0, decodeMs = 0;
    double lossPercent = 0;
    uint32_t framesDropped = 0;
    double encodeMs = 0, measuredFps = 0;
    bool usingVirtualDisplay = false;
    std::string sourceName;
};

}  // namespace sd
