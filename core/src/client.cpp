#include "sidedroid/client.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace sd {

namespace {
constexpr uint64_t kProbeSeqBase = 1ull << 63;  // keeps client probes out of the host's nonce space
}

Client::Client(ClientOptions opts, Callbacks cb) : opts_(std::move(opts)), cb_(std::move(cb)) {}

Client::~Client() { stop(); }

bool Client::start(std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    Socket s = Socket::tcpConnect(opts_.host, opts_.port, 5000);
    if (!s.valid()) return fail("connect");
    s.setBuffers(0, 4 * 1024 * 1024);

    std::function<Key32(const uint8_t*, const uint8_t*)> derive;
    if (opts_.mode == AuthMode::Pairing) {
        std::string code = opts_.pairingCode;
        derive = [code](const uint8_t* c, const uint8_t* h) { return pskFromPairingCode(code, c, h); };
    }
    auto hs = handshakeClient(s, opts_.mode, opts_.clientId, opts_.pairedSecret, derive);
    if (!hs) return fail("auth");
    keys_ = hs->keys;
    hostIp_ = s.peerIpv4();

    if (opts_.mode == AuthMode::Pairing && cb_.onPaired) cb_.onPaired(keys_.pairSecret);

    channel_ = std::make_unique<SecureChannel>(std::move(s), keys_.tcpC2H, keys_.tcpH2C);
    if (!channel_->send(encode(opts_.hello))) return fail("send");

    running_ = true;
    disconnected_ = false;

    if (opts_.hello.wantUdp) {
        udp_ = Socket::udpBind(0);
        if (!udp_.valid()) return fail("udp");
        udp_.setBuffers(0, 4 * 1024 * 1024);
        udp_.setRecvTimeout(5);
        FrameAssembler::Callbacks fc;
        fc.onFrame = [this](uint32_t, Bytes&& data) {
            winFrames_.fetch_add(1);
            handleFrameMessage(data.data(), data.size());
        };
        fc.onNack = [this](const Nack& n) { channel_->send(encode(n)); };
        fc.onDrop = [this](uint32_t) { onDatagramFrameDrop(); };
        assembler_ = std::make_unique<FrameAssembler>(std::move(fc));
        udpThread_ = std::thread(&Client::udpLoop, this);
    }
    tcpThread_ = std::thread(&Client::tcpLoop, this);
    statsThread_ = std::thread(&Client::statsLoop, this);
    return true;
}

void Client::stop() {
    running_ = false;
    if (channel_) channel_->shutdown();
    if (tcpThread_.joinable()) tcpThread_.join();
    if (udpThread_.joinable()) udpThread_.join();
    if (statsThread_.joinable()) statsThread_.join();
    udp_.close();
}

void Client::disconnect(const std::string& reason) {
    bool expected = false;
    if (!disconnected_.compare_exchange_strong(expected, true)) return;
    running_ = false;
    if (cb_.onDisconnected) cb_.onDisconnected(reason);
}

void Client::handleFrameMessage(const uint8_t* data, size_t size) {
    if (size < 1 || data[0] != static_cast<uint8_t>(MsgType::Frame)) return;
    FrameHeader h;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    if (!decodeFrame(Reader(data + 1, size - 1), h, payload, payloadSize)) return;

    winBytes_.fetch_add(static_cast<uint32_t>(size));

    // Host and client clocks differ, but the change of their difference is
    // exactly the growth of the queue, which is all the rate controller needs.
    int64_t delay = static_cast<int64_t>(monotonicUs()) - static_cast<int64_t>(h.captureUs);
    sumDelayUs_.fetch_add(delay);
    delaySamples_.fetch_add(1);

    // After a loss, P-frames reference data the decoder never had.
    if (awaitingKey_.load()) {
        if (!h.keyframe) return;
        awaitingKey_ = false;
    }
    if (cb_.onFrame) cb_.onFrame(h, payload, payloadSize);
}

void Client::onDatagramFrameDrop() {
    winDropped_.fetch_add(1);
    totalDropped_.fetch_add(1);
    if (!awaitingKey_.exchange(true)) {
        lastKeyRequestUs_ = monotonicUs();
        requestKeyframe();
    }
}

void Client::tcpLoop() {
    Bytes msg;
    while (running_) {
        if (!channel_->recv(msg)) {
            disconnect(running_ ? "connection lost" : "stopped");
            return;
        }
        if (msg.empty()) continue;
        auto type = static_cast<MsgType>(msg[0]);
        Reader body(msg.data() + 1, msg.size() - 1);
        switch (type) {
        case MsgType::Config:
            if (auto c = decodeConfig(body)) {
                // A new configuration always starts on a keyframe.
                awaitingKey_ = true;
                if (cb_.onConfig) cb_.onConfig(*c);
            }
            break;
        case MsgType::DisplayOptions:
            if (auto o = decodeDisplayOptions(body)) {
                if (cb_.onDisplayOptions) cb_.onDisplayOptions(*o);
            }
            break;
        case MsgType::Frame:
            winFrames_.fetch_add(1);
            handleFrameMessage(msg.data(), msg.size());
            break;
        case MsgType::UdpSetup:
            if (auto u = decodeUdpSetup(body)) {
                hostUdpPort_ = u->port;
                udpReady_ = true;
            }
            break;
        case MsgType::Ping:
            channel_->send(encodePing(MsgType::Pong, body.u64()));
            break;
        case MsgType::Pong: {
            uint64_t sent = body.u64();
            if (body.ok()) rttUs_ = static_cast<uint32_t>(monotonicUs() - sent);
            break;
        }
        case MsgType::Bye:
        {
            const uint8_t why = body.u8();
            disconnect(body.ok() && why == kByeHostFull ? "host full" : "host closed");
        }
            return;
        default:
            break;
        }
    }
}

void Client::udpLoop() {
    Bytes buf(2048), plain(2048);
    ReplayWindow replay;
    uint64_t probeSeq = kProbeSeqBase;
    uint64_t lastProbe = 0;
    uint64_t highestSeq = 0;
    bool haveSeq = false;
    uint32_t rng = 0x12345678;
    double tokens = 0;
    uint64_t lastRefill = monotonicUs();
    while (running_) {
        uint64_t now = monotonicUs();
        // Keeps the path open and tells the host where to send.
        if (udpReady_ && now - lastProbe > 200000) {
            lastProbe = now;
            uint8_t probe[kUdpOverhead + 4];
            const uint8_t body[4] = {'P', 'R', 'O', 'B'};
            udpSeal(keys_.udpH2C, probeSeq++, body, 4, probe);
            udp_.sendTo(probe, sizeof(probe), hostIp_, hostUdpPort_);
        }
        int n = udp_.recvFrom(buf.data(), buf.size(), nullptr, nullptr);
        now = monotonicUs();
        if (n > 0 && opts_.debugDropPercent > 0) {
            rng = rng * 1664525u + 1013904223u;
            if ((rng >> 16) % 100 < static_cast<uint32_t>(opts_.debugDropPercent)) n = 0;
        }
        if (opts_.debugLimitKbps > 0) {
            const double bytesPerUs = opts_.debugLimitKbps / 8000.0;
            tokens = (std::min)(tokens + (now - lastRefill) * bytesPerUs, 64000.0);
            lastRefill = now;
            if (n > 0) {
                if (tokens < n) n = 0;  // tail drop, like a full queue on a slow link
                else tokens -= n;
            }
        }
        if (n > 0 && opts_.debugSharedLink && !opts_.debugSharedLink(n)) n = 0;
        if (n > static_cast<int>(kUdpOverhead)) {
            uint64_t seq;
            if (udpOpen(keys_.udpH2C, buf.data(), static_cast<size_t>(n), plain.data(), &seq) &&
                seq < kProbeSeqBase && replay.accept(seq)) {
                winPackets_.fetch_add(1);
                // Loss is a gap in the sender's sequence, seen before FEC or NACK
                // repairs it. Parity shards that simply arrive after their frame
                // completed are not loss.
                if (!haveSeq) {
                    haveSeq = true;
                    highestSeq = seq;
                } else if (seq > highestSeq) {
                    winLost_.fetch_add(static_cast<uint32_t>(seq - highestSeq - 1));
                    highestSeq = seq;
                } else if (winLost_.load() > 0) {
                    winLost_.fetch_sub(1);  // arrived out of order after all
                }
                assembler_->onPacket(plain.data(), static_cast<size_t>(n) - kUdpOverhead, now);
            }
        } else if (n < 0) {
            disconnect("udp failed");
            return;
        }
        assembler_->poll(now);
    }
}

void Client::statsLoop() {
    uint64_t lastUs = monotonicUs();
    uint64_t lastPing = 0;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t now = monotonicUs();
        if (now - lastPing >= 1000000) {
            lastPing = now;
            channel_->send(encodePing(MsgType::Ping, now));
        }
        Stats s;
        s.windowMs = static_cast<uint32_t>((now - lastUs) / 1000);
        lastUs = now;
        s.bytes = winBytes_.exchange(0);
        s.frames = winFrames_.exchange(0);
        s.packetsReceived = winPackets_.exchange(0);
        s.packetsLost = winLost_.exchange(0);
        s.framesDropped = winDropped_.exchange(0);
        // The keyframe itself can be lost. Keep asking, or the picture stays frozen forever.
        if (assembler_ && awaitingKey_.load() && now - lastKeyRequestUs_.load() >= 500000) {
            lastKeyRequestUs_ = now;
            requestKeyframe();
        }
        uint32_t samples = delaySamples_.exchange(0);
        int64_t sum = sumDelayUs_.exchange(0);
        if (samples > 0) {
            int64_t avg = sum / samples;
            int64_t prev = lastAvgDelayUs_.exchange(avg);
            if (prev != 0) s.delayGradientUs = static_cast<int32_t>(avg - prev);
        }
        s.decodeUs = decodeUs_.load();
        s.rttUs = rttUs_.load();
        channel_->send(encode(s));
    }
}

void Client::sendTouch(const std::vector<TouchPoint>& pts) {
    if (channel_ && running_) channel_->send(encodeTouch(pts));
}

void Client::sendViewport(uint16_t w, uint16_t h) {
    if (channel_ && running_) channel_->send(encode(Viewport{w, h}));
}

void Client::sendSetDisplay(const SetDisplay& s) {
    if (channel_ && running_) channel_->send(encode(s));
}

void Client::requestKeyframe() {
    if (channel_ && running_) channel_->send(encodeSimple(MsgType::KeyframeRequest));
}

void Client::reportDecodeUs(uint32_t us) { decodeUs_ = us; }

}  // namespace sd
