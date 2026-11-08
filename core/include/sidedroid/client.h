// Client side of a session: connects, authenticates, receives frames over TCP
// or UDP and reports statistics. Shared by the Android app and the PC loopback
// test tool, so both exercise exactly the same protocol code.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "sidedroid/delaystats.h"
#include "sidedroid/fec.h"
#include "sidedroid/secure.h"

namespace sd {

struct ClientOptions {
    std::string host = "127.0.0.1";
    uint16_t port = kDefaultPort;
    AuthMode mode = AuthMode::Usb;
    ClientId clientId{};
    Key32 pairedSecret{};        // AuthMode::Paired
    std::string pairingCode;     // AuthMode::Pairing
    Hello hello;
    // Test tool only: simulate a bad network on the receiving end.
    int debugDropPercent = 0;
    uint32_t debugLimitKbps = 0;
    // Testing only: decides whether a packet of this size gets through, as a link several clients share would.
    std::function<bool(int)> debugSharedLink;
};

class Client {
public:
    struct Callbacks {
        std::function<void(const Config&)> onConfig;
        std::function<void(const DisplayOptions&)> onDisplayOptions;
        // Runs on the network thread. data is valid only during the call.
        std::function<void(const FrameHeader&, const uint8_t* data, size_t size)> onFrame;
        std::function<void(const std::string& reason)> onDisconnected;
        // Called after a successful pairing, to persist the secret.
        std::function<void(const Key32& secret)> onPaired;
    };

    Client(ClientOptions opts, Callbacks cb);
    ~Client();

    // Blocks through connect and handshake. Returns false with `error` set.
    bool start(std::string* error);
    void stop();

    void sendTouch(const std::vector<TouchPoint>& pts);
    void sendViewport(uint16_t w, uint16_t h);
    void sendSetDisplay(const SetDisplay& s);
    void requestKeyframe();
    void reportDecodeUs(uint32_t us);
    // Reports decoder overload; configuration and surface resets are not drops.
    void reportDecoderDrop();

    uint32_t lastRttUs() const { return rttUs_.load(); }
    uint64_t totalFramesDropped() const { return totalDropped_.load(); }

private:
    void tcpLoop();
    void udpLoop();
    void statsLoop();
    void handleFrameMessage(const uint8_t* data, size_t size);
    void disconnect(const std::string& reason);
    void onDatagramFrameDrop();

    ClientOptions opts_;
    Callbacks cb_;
    std::unique_ptr<SecureChannel> channel_;
    SessionKeys keys_{};
    Socket udp_;
    uint32_t hostIp_ = 0;
    uint16_t hostUdpPort_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> udpReady_{false};
    std::atomic<bool> disconnected_{false};
    std::thread tcpThread_, udpThread_, statsThread_;

    std::unique_ptr<FrameAssembler> assembler_;
    std::atomic<bool> awaitingKey_{true};
    std::atomic<uint64_t> lastKeyRequestUs_{0};
    std::atomic<uint64_t> totalDropped_{0};

    // Window counters, reset by the stats thread.
    std::atomic<uint32_t> winBytes_{0}, winFrames_{0}, winPackets_{0}, winLost_{0}, winDropped_{0};
    std::atomic<uint32_t> decodeUs_{0}, rttUs_{0};
    DelayStats delayStats_;
};

}  // namespace sd
