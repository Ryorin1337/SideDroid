// The host application: accepts up to kMaxDevices devices, each with its own display
// and video pipeline.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
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
#include "updater.h"

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
    // Test runs only: these kinds of encoder fail after a few frames, or this one ignores
    // keyframe requests.
    std::string testBreakEncoder, testDeafEncoder;
    // Test runs only: look for updates here, and verify them without installing.
    std::wstring updateTestBase;
};

class Session;

constexpr size_t kMaxDevices = 4;

struct LiveStatus {
    bool connected = false;
    std::string id;  // client id in hex, or a name for this connection when it has none
    std::string device, transport, encoder, codec, adapter, error;
    int displayW = 0, displayH = 0, hz = 0, encodeW = 0, encodeH = 0, fps = 0;
    uint32_t kbps = 0, measuredKbps = 0, rttMs = 0, decodeMs = 0;
    double lossPercent = 0;
    uint32_t framesDropped = 0;
    double encodeMs = 0, measuredFps = 0;
    bool usingVirtualDisplay = false;
    std::string captureGpu, encoderGpu;
    bool crossAdapter = false;     // the picture travels through system memory between them
    bool encoderFallback = false;  // the chosen encoder could not be used
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

    // For Session. Everything that changes displays happens under this lock, because
    // Windows takes a layout as a whole and the sessions would overwrite each other.
    std::unique_lock<std::recursive_mutex> lockDisplays() { return std::unique_lock<std::recursive_mutex>(displayMutex_); }
    // Remembers the current layout, once, before the first change to it.
    bool saveLayoutBeforeChange();
    // The monitor of the driver this session shows, adding one if all are taken. A device
    // back within the grace period gets the same one. Empty on failure.
    std::wstring claimDisplay(const Session* session, const ClientId& id);
    // Monitors with a device behind them, or waiting for one to come back.
    std::vector<std::wstring> claimedDisplays();
    // Makes the driver offer this mode, with a monitor for every device. The first device turns
    // the driver on, and if it is on already and has to read a change, it restarts. Returns the
    // problem to show, if any: without the service, only what the driver has now is available.
    const char* prepareDriver(const Session* session, const DisplayMode& want, bool* changedDriver);
    // With a capture open on a monitor, Windows puts off restarting the device until the computer
    // restarts (ERROR_SUCCESS_RESTART_REQUIRED). Stops the captures of the other sessions, which
    // then wait for the display lock. Call with the lock held. False if one does not stop in time.
    bool stopVirtualCaptures(const Session* self);
    // Lets go of it after the grace period, or at once.
    void releaseDisplay(const Session* session, bool now);

    // All connected devices, oldest first.
    std::vector<LiveStatus> liveStatuses();
    // The oldest device, or just the host's own error if none is connected.
    LiveStatus liveStatus();
    // New defaults, for every device. What was chosen for one device in particular ends here.
    void applySettings(const Settings& s);
    void setAllowWireless(bool allow);
    // Which GPU renders the virtual displays, by name. Empty for Windows' choice. All devices
    // pause while the driver restarts. False if there is no such GPU.
    bool setVddGpu(const std::string& name);
    // Empty id: every device.
    void disconnectPhone(const std::string& id = {});
    std::string startPairing();
    Settings currentSettings() { return store_.settings(); }
    // Each connected device as it is now, oldest first, for the tray menu.
    std::vector<std::pair<LiveStatus, DeviceSettings>> deviceViews();
    // What was chosen for this one device. The fields of `s` other than the display's are not used.
    void chooseForDevice(const std::string& id, const Settings& s);
    // True when SideDroid installed the driver, so the display is ours to manage.
    bool ownsDriver() const { return ownsDriver_; }
    UpdateStatus updateStatus() { return updater_ ? updater_->status() : UpdateStatus{}; }
    bool installUpdate() { return updater_ && updater_->install(); }

private:
    // One accepted socket, from the handshake until it is gone.
    struct Connection {
        std::thread thread;
        ClientId clientId{};
        bool holdsPlace = false;  // counts towards kMaxDevices
        std::shared_ptr<Session> session;
        std::atomic<bool> done{false};
    };
    void acceptLoop();
    void serveConnection(Socket s, std::string peer, bool loopback, Connection* conn);
    void reapConnections();
    bool takePlace(Connection* conn, const ClientId& id);
    std::string statusJson();
    void detachIfIdle(bool force);
    void restoreLayoutNow();
    // Puts the layout back and destroys the virtual displays: nothing is using them any more.
    void lastDisplayGone();
    void dropClaim(size_t index);
    bool consoleStartsWithWindows();
    void setConsoleStartsWithWindows(bool on);
    void setHostError(const std::string& error, bool onlyIfSame = false);
    bool ensureListening();
    void recoverLayoutAtStart();
    std::optional<std::string> takePairingCodeAttempt();

    HostOptions opts_;
    Store store_;
    AdbWatcher adb_;
    Discovery discovery_;
    std::unique_ptr<IpcServer> ipc_;
    std::unique_ptr<Updater> updater_;
    Socket listener_;
    int listenFailures_ = 0;
    std::atomic<bool> running_{false};
    bool ownsDriver_ = false;
    std::thread acceptThread_;

    std::mutex sessionMutex_;
    std::condition_variable sessionCv_;
    std::list<std::unique_ptr<Connection>> connections_;

    std::mutex pairMutex_;
    std::string pairingCode_;
    uint64_t pairingExpiresUs_ = 0;
    int pairingAttempts_ = 0;
    std::string lastPairedName_;

    std::mutex statusMutex_;
    std::string hostError_;

    // Which device shows which monitor. Without a session, it waits until releaseAtUs.
    struct DisplayClaim {
        std::wstring monitor;
        const Session* session = nullptr;
        ClientId owner{};
        uint64_t releaseAtUs = 0;
    };
    std::recursive_mutex displayMutex_;
    std::vector<DisplayClaim> claims_;
    bool layoutSaved_ = false;
    DisplayLayout savedLayout_;
    bool driverOn_ = false;
    std::vector<std::pair<const Session*, DisplayMode>> modesInUse_;
};


}  // namespace sd
