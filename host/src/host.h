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
#include <vector>

#include "adb.h"
#include "discovery.h"
#include "input.h"
#include "ipc.h"
#include "sidedroid/fec.h"
#include "sidedroid/ratecontrol.h"
#include "sidedroid/secure.h"
#include "store.h"
#include "topology.h"

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
    bool useVirtualDisplayInTest = false;  // full display handling on a test port
};

class Session;

struct LiveStatus {
    bool connected = false;
    std::string device, transport, encoder, codec, adapter, error;
    int displayW = 0, displayH = 0, hz = 0, encodeW = 0, encodeH = 0, fps = 0;
    uint32_t kbps = 0, measuredKbps = 0, rttMs = 0, decodeMs = 0;
    double lossPercent = 0;
    uint32_t framesDropped = 0;
    double encodeMs = 0, measuredFps = 0;
    bool usingVirtualDisplay = false;
    uint16_t phoneW = 0, phoneH = 0;
    uint16_t maxDecodeLong = 0, maxDecodeShort = 0;
    uint16_t recommendedScale = 0, currentScale = 0;
    std::vector<uint16_t> phoneRates;
    std::string sourceName;
};

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
    void publishStatus(const std::function<void(LiveStatus&)>& edit);
    // Remembers the current layout, once, before the first change to it.
    bool saveLayoutBeforeChange();
    // Puts the saved layout back right away, for switching to mirroring.
    void restoreLayoutNow();
    LiveStatus liveStatus();
    void applySettings(const Settings& s);
    void disconnectPhone();
    std::string startPairing();
    Settings currentSettings() { return store_.settings(); }
    // True when SideDroid installed the driver, so the display is ours to manage.
    bool ownsDriver() const { return ownsDriver_; }

private:
    void acceptLoop();
    std::string statusJson();
    void detachIfIdle(bool force);
    void recoverLayoutAtStart();
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
    std::unique_ptr<LiveStatus> live_;

    // Virtual display attached by us and waiting to be taken off the desktop.
    std::mutex attachMutex_;
    bool layoutSaved_ = false;
    DisplayLayout savedLayout_;
    uint64_t detachAtUs_ = 0;
};


}  // namespace sd
