#include "host.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "capture.h"
#include "display.h"
#include "encoder.h"
#include "log.h"

namespace sd {

namespace {
constexpr uint64_t kPairingLifetimeUs = 120'000'000;
constexpr int kPairingMaxAttempts = 5;
constexpr uint64_t kDetachGraceUs = 5'000'000;
constexpr uint64_t kIdleKeepAliveUs = 500'000;
constexpr int kRepeatAfterChange = 6;  // flushes decoders that hold on to a few frames
constexpr size_t kUdpProbeBytes = 4 + kUdpOverhead;
constexpr uint64_t kProbeSeqBase = 1ull << 63;

const char* codecName(uint8_t c) { return c == kCodecHevc ? "HEVC" : "H.264"; }

bool sameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

}  // namespace

// ---------------------------------------------------------------------------
// One connected phone.
// ---------------------------------------------------------------------------
class Session {
public:
    Session(Host& host, Socket&& sock, const HandshakeResult& hs, bool loopback)
        : host_(host), keys_(hs.keys), mode_(hs.mode), clientId_(hs.clientId), loopback_(loopback) {
        peerIp_ = sock.peerIpv4();
        sock.setBuffers(512 * 1024, 0);
        channel_ = std::make_unique<SecureChannel>(std::move(sock), keys_.tcpH2C, keys_.tcpC2H);
    }

    void run();
    void requestStop() {
        running_ = false;
        channel_->shutdown();
        queueCv_.notify_all();
    }
    void settingsChanged() { displayDirty_ = true; }
    const std::string& deviceName() const { return hello_.deviceName; }

private:
    void videoLoop();
    void senderLoop();
    void recvLoop();
    void enqueue(Bytes&& msg, bool key);
    double backlogMs();
    void sendUdpFrame(uint32_t id, const Bytes& msg);
    void handleNack(const Nack& n);
    bool prepareOutput(std::wstring* device, bool* isVirtual, DisplayMode* mode);
    uint32_t bitrateCap(const DisplayMode& m) const;

    Host& host_;
    SessionKeys keys_;
    AuthMode mode_;
    ClientId clientId_;
    bool loopback_;
    uint32_t peerIp_ = 0;
    std::unique_ptr<SecureChannel> channel_;
    Hello hello_;

    std::atomic<bool> running_{true};
    std::atomic<bool> displayDirty_{false};
    std::atomic<bool> keyRequested_{true};
    std::atomic<uint16_t> viewportW_{0}, viewportH_{0};

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    struct Queued { Bytes msg; bool key; };
    std::deque<Queued> queue_;
    size_t queueBytes_ = 0;
    uint32_t frameId_ = 0;

    // UDP
    bool useUdp_ = false;
    Socket udp_;
    std::atomic<uint32_t> clientUdpIp_{0};
    std::atomic<uint16_t> clientUdpPort_{0};
    std::atomic<uint64_t> udpSeq_{0};
    std::mutex retransmitMutex_;
    RetransmitBuffer retransmit_;
    std::thread udpRecvThread_;

    // Rate control
    std::mutex rateMutex_;
    std::unique_ptr<RateController> rate_;
    RateTarget target_;
    std::atomic<bool> targetDirty_{false};
    std::atomic<uint32_t> kbpsNow_{0};

    std::unique_ptr<TouchInjector> touch_;
    std::mutex touchMutex_;
};

uint32_t Session::bitrateCap(const DisplayMode& m) const {
    auto s = host_.currentSettings();
    if (s.maxKbps) return s.maxKbps;
    // About 0.12 bits per pixel, which is clean for desktop content in HEVC,
    // with a tighter ceiling on shared radio than on a USB cable.
    double pixelsPerSecond = static_cast<double>(m.width) * m.height * m.hz;
    auto cap = static_cast<uint32_t>(pixelsPerSecond * 0.12 / 1000);
    return std::clamp<uint32_t>(cap, 8000, useUdp_ ? 60000 : 100000);
}

void Session::enqueue(Bytes&& msg, bool key) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    // When the path cannot keep up, stale pictures only add latency. Discard
    // them and restart from a keyframe, since later P-frames depend on them.
    const uint32_t kbps = kbpsNow_.load();
    const double ms = kbps ? queueBytes_ * 8.0 / kbps : 0;
    if (ms > 200 && !key) {
        queue_.clear();
        queueBytes_ = 0;
        keyRequested_ = true;
        return;
    }
    queueBytes_ += msg.size();
    queue_.push_back({std::move(msg), key});
    queueCv_.notify_one();
}

double Session::backlogMs() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    const uint32_t kbps = kbpsNow_.load();
    return kbps ? queueBytes_ * 8.0 / kbps : 0;
}

void Session::senderLoop() {
    while (running_) {
        Queued q;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [&] { return !running_ || !queue_.empty(); });
            if (!running_) return;
            q = std::move(queue_.front());
            queue_.pop_front();
            queueBytes_ -= q.msg.size();
        }
        if (useUdp_) {
            // Until the phone's first probe arrives there is nowhere to send to.
            if (clientUdpIp_.load() != 0) sendUdpFrame(frameId_++, q.msg);
        } else if (!channel_->send(q.msg)) {
            requestStop();
            return;
        }
    }
}

void Session::sendUdpFrame(uint32_t id, const Bytes& msg) {
    uint32_t ip = clientUdpIp_.load();
    uint16_t port = clientUdpPort_.load();
    if (ip == 0) return;

    int parity, kbps;
    {
        std::lock_guard<std::mutex> lock(rateMutex_);
        parity = target_.parityPercent;
        kbps = static_cast<int>(target_.bitrateKbps);
    }
    auto packets = packetize(id, msg.data(), msg.size(), parity);
    {
        std::lock_guard<std::mutex> lock(retransmitMutex_);
        retransmit_.store(id, packets);
    }

    // Pace at about three times the mean rate. A keyframe sent as one burst
    // overruns Wi-Fi queues and turns into exactly the loss we want to avoid.
    const double usPerPacket = (kShardPayload + kPacketHeader) * 8.0 * 1000.0 / std::max(1000, kbps * 3);
    Bytes sealed(kPacketHeader + kShardPayload + kUdpOverhead);
    uint64_t start = monotonicUs();
    for (size_t i = 0; i < packets.size() && running_; ++i) {
        udpSeal(keys_.udpH2C, udpSeq_.fetch_add(1), packets[i].data(), packets[i].size(), sealed.data());
        udp_.sendTo(sealed.data(), packets[i].size() + kUdpOverhead, ip, port);
        const uint64_t due = start + static_cast<uint64_t>(usPerPacket * (i + 1));
        const uint64_t now = monotonicUs();
        if (due > now + 1500) Sleep(1);
    }
}

void Session::handleNack(const Nack& n) {
    uint32_t ip = clientUdpIp_.load();
    uint16_t port = clientUdpPort_.load();
    if (ip == 0) return;
    Bytes sealed(kPacketHeader + kShardPayload + kUdpOverhead);
    for (const auto& it : n.items) {
        Bytes p;
        {
            std::lock_guard<std::mutex> lock(retransmitMutex_);
            p = retransmit_.find(n.frameId, it.block, it.shard);
        }
        if (p.empty()) continue;
        udpSeal(keys_.udpH2C, udpSeq_.fetch_add(1), p.data(), p.size(), sealed.data());
        udp_.sendTo(sealed.data(), p.size() + kUdpOverhead, ip, port);
    }
}

bool Session::prepareOutput(std::wstring* device, bool* isVirtual, DisplayMode* mode) {
    const auto& opts = host_.options();
    auto s = host_.currentSettings();
    SetDisplay manual{s.width, s.height, s.hz, s.maxKbps};
    DisplayMode want = chooseMode(hello_, viewportW_.load(), viewportH_.load(), manual);
    *mode = want;

    if (!opts.sourceOverride.empty()) {
        *device = opts.sourceOverride;
        *isVirtual = false;
        return true;
    }

    auto vdd = findVddOutput();
    if (!vdd) {
        host_.publishStatus([](LiveStatus& l) { l.error = "virtual_display_missing"; });
        logf("virtual display driver not found");
        return false;
    }
    *device = vdd->deviceName;
    *isVirtual = true;

    if (!outputHasMode(*device, want)) {
        // First sight of this phone's resolution: teach it to the driver.
        std::vector<DisplayMode> modes{want, {want.height, want.width, want.hz}};
        bool changed = false;
        if (!ensureVddModes(modes, &changed)) {
            host_.publishStatus([](LiveStatus& l) { l.error = "virtual_display_config_denied"; });
        } else if (changed) {
            logf("added %dx%d@%d to driver config; reloading", want.width, want.height, want.hz);
            vddCommand(L"RELOAD_DRIVER");
            for (int i = 0; i < 50 && running_ && !outputHasMode(*device, want); ++i) {
                Sleep(100);
                if (auto v = findVddOutput()) *device = v->deviceName;
            }
        }
        if (!outputHasMode(*device, want)) {
            // Keep the session useful: closest mode of the same orientation that exists.
            auto modes2 = listModes(*device);
            const bool portrait = want.height > want.width;
            std::optional<DisplayMode> best;
            int bestScore = INT_MAX;
            for (const auto& m : modes2) {
                if ((m.height > m.width) != portrait) continue;
                int score = abs(m.width - want.width) + abs(m.height - want.height) + abs(m.hz - want.hz) * 4;
                if (score < bestScore) { bestScore = score; best = m; }
            }
            if (!best) {
                logf("no usable display mode for %dx%d@%d", want.width, want.height, want.hz);
                return false;
            }
            logf("using closest mode %dx%d@%d", best->width, best->height, best->hz);
            *mode = *best;
        }
    }
    if (!attachOutput(*device, *mode)) return false;
    host_.markOutputAttached(*device);
    return true;
}

// Noise tiles moving over the picture: hard to compress and independent of what
// is on screen, so bitrate tests are repeatable and show no user content.
static void drawTestPattern(DesktopCapture& capture, uint32_t tick) {
    constexpr UINT kTile = 256;
    static std::vector<uint32_t> noise = [] {
        std::vector<uint32_t> n(kTile * kTile + 1024);
        uint32_t r = 12345;
        for (auto& p : n) {
            r = r * 1664525u + 1013904223u;
            p = 0xFF000000u | (r >> 8);
        }
        return n;
    }();
    auto* ctx = capture.context();
    const UINT w = capture.width(), h = capture.height();
    if (w <= kTile || h <= kTile) return;
    for (uint32_t i = 0; i < 24; ++i) {
        UINT x = static_cast<UINT>((tick * (7 + i) + i * 397) % (w - kTile));
        UINT y = static_cast<UINT>((tick * (5 + i % 3) + i * 211) % (h - kTile));
        D3D11_BOX box{x, y, 0, x + kTile, y + kTile, 1};
        ctx->UpdateSubresource(capture.texture(), 0, &box, noise.data() + (tick + i) % 1024, kTile * 4, 0);
    }
}

void Session::videoLoop() {
    int openFailures = 0;
    while (running_) {
        displayDirty_ = false;

        std::wstring device;
        bool isVirtual = false;
        DisplayMode mode;
        if (!prepareOutput(&device, &isVirtual, &mode)) {
            Sleep(1000);
            continue;
        }

        // Windows needs a moment after a mode change before duplication succeeds.
        DesktopCapture capture;
        bool opened = false;
        for (int i = 0; i < 40 && running_ && !displayDirty_; ++i) {
            if (capture.open(device)) { opened = true; break; }
            Sleep(100);
        }
        if (!opened) {
            if (++openFailures % 5 == 1) logf("cannot capture %s", narrow(device).c_str());
            Sleep(500);
            continue;
        }
        openFailures = 0;

        const int capW = capture.width(), capH = capture.height();
        if (auto o = findOutputByName(device); o && host_.options().enableTouch) {
            std::lock_guard<std::mutex> lock(touchMutex_);
            if (touch_) touch_->setDisplayRect(o->rect);
        }

        // The real rate is what the capture delivers, whatever was requested.
        RateTarget startTarget;
        RateLimits limits;
        limits.udp = useUdp_;
        limits.maxKbps = bitrateCap(mode);
        limits.minKbps = std::min<uint32_t>(limits.maxKbps, 1500);
        limits.startKbps = std::min<uint32_t>(limits.maxKbps, useUdp_ ? 12000 : 30000);
        limits.maxFps = std::max<int>(mode.hz, 30);
        limits.minFps = 30;
        {
            std::lock_guard<std::mutex> lock(rateMutex_);
            if (!rate_) rate_ = std::make_unique<RateController>(limits);
            else rate_->setLimits(limits);
            target_ = rate_->target();
            targetDirty_ = false;
            startTarget = target_;
        }
        kbpsNow_ = startTarget.bitrateKbps;

        // Inner loop runs until something forces a rebuild of the pipeline.
        const int scale = startTarget.scalePercent;
        const int encW = std::max(320, capW * scale / 100) & ~1;
        const int encH = std::max(240, capH * scale / 100) & ~1;
        const int fps = startTarget.fps;

        uint8_t codec = (hello_.codecs & kCodecHevc) ? kCodecHevc : kCodecH264;

        // Same adapter first: no copy through system memory. Then any other adapter.
        ComPtr<ID3D11Device> encDev = capture.device();
        ComPtr<ID3D11DeviceContext> encCtx = capture.context();
        std::unique_ptr<Encoder> encoder;
        auto tryOn = [&](ID3D11Device* dev) {
            EncoderConfig ec;
            ec.device = dev;
            ec.width = encW;
            ec.height = encH;
            ec.fps = fps;
            ec.kbps = startTarget.bitrateKbps;
            for (uint8_t c : {codec, static_cast<uint8_t>(kCodecH264)}) {
                if (!(hello_.codecs & c)) continue;
                ec.codec = c;
                encoder = createNvencEncoder(ec);
                if (!encoder) encoder = createMfEncoder(ec);
                if (encoder) { codec = c; return true; }
            }
            return false;
        };
        bool crossAdapter = false;
        if (!tryOn(capture.device())) {
            for (const auto& [luid, vendor] : listHardwareAdapters()) {
                if (sameLuid(luid, capture.adapterLuid())) continue;
                ComPtr<ID3D11DeviceContext> ctx;
                auto dev = createDeviceOnAdapter(luid, &ctx);
                if (dev && tryOn(dev.Get())) {
                    encDev = dev;
                    encCtx = ctx;
                    crossAdapter = true;
                    break;
                }
            }
        }
        if (!encoder) {
            logf("no hardware encoder available");
            host_.publishStatus([](LiveStatus& l) { l.error = "no_encoder"; });
            Sleep(2000);
            continue;
        }

        CrossAdapterCopy cross;
        if (crossAdapter && !cross.init(capture.device(), capture.context(), encDev.Get(), encCtx.Get(), capW, capH)) {
            logf("cross-adapter copy init failed");
            Sleep(1000);
            continue;
        }
        Nv12Converter conv;
        if (!conv.init(encDev.Get(), encCtx.Get(), capW, capH, encW, encH)) {
            logf("video processor init failed");
            Sleep(1000);
            continue;
        }

        logf("pipeline: %dx%d -> %dx%d @%d %s %s %u kbps%s", capW, capH, encW, encH, fps, encoder->name(),
             codecName(codec), startTarget.bitrateKbps, crossAdapter ? " (cross-adapter)" : "");

        Config cfg;
        cfg.displayWidth = static_cast<uint16_t>(capW);
        cfg.displayHeight = static_cast<uint16_t>(capH);
        cfg.encodeWidth = static_cast<uint16_t>(encW);
        cfg.encodeHeight = static_cast<uint16_t>(encH);
        cfg.fps = static_cast<uint16_t>(fps);
        cfg.codec = codec;
        cfg.bitrateKbps = startTarget.bitrateKbps;
        if (!channel_->send(encode(cfg))) { requestStop(); return; }
        keyRequested_ = true;

        host_.publishStatus([&](LiveStatus& l) {
            l.error.clear();
            l.displayW = capW;
            l.displayH = capH;
            l.hz = mode.hz;
            l.encodeW = encW;
            l.encodeH = encH;
            l.fps = fps;
            l.encoder = encoder->name();
            l.codec = codecName(codec);
            l.kbps = startTarget.bitrateKbps;
            l.usingVirtualDisplay = isVirtual;
            l.sourceName = narrow(device);
        });

        const uint64_t frameIntervalUs = 1'000'000 / std::max(fps, 1);
        uint64_t lastEncodeUs = 0, lastStatsUs = monotonicUs();
        bool unsent = false;
        int repeats = 0;
        uint64_t encodeSumUs = 0, encodeCount = 0;
        uint32_t appliedKbps = startTarget.bitrateKbps;
        uint32_t patternTick = 0;

        while (running_ && !displayDirty_) {
            if (targetDirty_.exchange(false)) {
                RateTarget t;
                {
                    std::lock_guard<std::mutex> lock(rateMutex_);
                    t = target_;
                }
                if (t.scalePercent != scale || t.fps != fps) break;  // rebuild
                if (t.bitrateKbps != appliedKbps && encoder->setBitrate(t.bitrateKbps)) {
                    appliedKbps = t.bitrateKbps;
                    kbpsNow_ = appliedKbps;
                    host_.publishStatus([&](LiveStatus& l) { l.kbps = appliedKbps; });
                }
            }

            auto r = capture.acquire(static_cast<int>(std::max<uint64_t>(frameIntervalUs / 1000, 1)));
            if (r == CaptureResult::Lost) {
                logf("capture lost (mode change or desktop switch)");
                break;
            }
            if (r == CaptureResult::NewFrame) {
                unsent = true;
                repeats = kRepeatAfterChange;
            }

            if (host_.options().testPattern) {
                drawTestPattern(capture, patternTick++);
                unsent = true;
            }

            const uint64_t now = monotonicUs();
            const bool dueByRate = now - lastEncodeUs >= frameIntervalUs * 9 / 10;
            const bool wantRepeat = repeats > 0 && dueByRate;
            const bool keepAlive = now - lastEncodeUs >= kIdleKeepAliveUs;
            if (keyRequested_.load() || (unsent && dueByRate) || wantRepeat || keepAlive) {
                ID3D11Texture2D* tex = capture.texture();
                if (crossAdapter) {
                    tex = cross.copy(tex);
                    if (!tex) break;
                }
                if (!conv.convert(tex)) {
                    logf("colour conversion failed");
                    break;
                }
                const bool force = keyRequested_.exchange(false);
                const uint64_t captureUs = now;
                bool encoded = encoder->encode(conv.output(), force, [&](const uint8_t* data, size_t size, bool key) {
                    FrameHeader fh;
                    fh.captureUs = captureUs;
                    fh.keyframe = key;
                    static std::atomic<uint32_t> seq{0};
                    fh.frameId = seq.fetch_add(1);
                    enqueue(encodeFrame(fh, data, size), key);
                });
                if (!encoded) {
                    logf("encode failed; rebuilding");
                    Sleep(100);
                    break;
                }
                encodeSumUs += monotonicUs() - now;
                ++encodeCount;
                lastEncodeUs = now;
                if (!unsent && repeats > 0) --repeats;
                unsent = false;
            }

            if (now - lastStatsUs > 1'000'000) {
                double avgMs = encodeCount ? encodeSumUs / 1000.0 / encodeCount : 0;
                double seconds = (now - lastStatsUs) / 1e6;
                double measured = encodeCount / seconds;
                host_.publishStatus([&](LiveStatus& l) {
                    l.encodeMs = avgMs;
                    l.measuredFps = measured;
                });
                encodeSumUs = 0;
                encodeCount = 0;
                lastStatsUs = now;
            }
        }
    }
}

void Session::recvLoop() {
    Bytes msg;
    uint64_t statsWindowBytes = 0;
    (void)statsWindowBytes;
    while (running_) {
        if (!channel_->recv(msg)) break;
        if (msg.empty()) continue;
        Reader body(msg.data() + 1, msg.size() - 1);
        switch (static_cast<MsgType>(msg[0])) {
        case MsgType::Touch:
            if (auto t = decodeTouch(body)) {
                std::lock_guard<std::mutex> lock(touchMutex_);
                if (touch_) touch_->inject(*t);
            }
            break;
        case MsgType::Stats:
            if (auto s = decodeStats(body)) {
                double backlog = useUdp_ ? 0 : backlogMs();
                bool changed = false;
                RateTarget t;
                {
                    std::lock_guard<std::mutex> lock(rateMutex_);
                    if (rate_) {
                        t = rate_->update(*s, backlog);
                        changed = !(t == target_);
                        target_ = t;
                    }
                }
                if (changed) targetDirty_ = true;
                uint32_t total = s->packetsReceived + s->packetsLost;
                const double loss = total ? 100.0 * s->packetsLost / total : 0;
                const double kbpsSeen = s->windowMs ? s->bytes * 8.0 / s->windowMs : 0;
                host_.publishStatus([&](LiveStatus& l) {
                    l.lossPercent = l.lossPercent * 0.8 + loss * 0.2;
                    l.measuredKbps = static_cast<uint32_t>(l.measuredKbps * 0.8 + kbpsSeen * 0.2);
                    l.rttMs = s->rttUs / 1000;
                    l.decodeMs = s->decodeUs / 1000;
                    l.framesDropped += s->framesDropped;
                });
            }
            break;
        case MsgType::KeyframeRequest:
            keyRequested_ = true;
            break;
        case MsgType::Viewport:
            if (auto v = decodeViewport(body)) {
                viewportW_ = v->width;
                viewportH_ = v->height;
                displayDirty_ = true;
            }
            break;
        case MsgType::SetDisplay:
            if (auto d = decodeSetDisplay(body)) {
                Settings s = host_.currentSettings();
                s.width = d->width;
                s.height = d->height;
                s.hz = d->hz;
                s.maxKbps = d->maxBitrateKbps;
                host_.store().setSettings(s);
                displayDirty_ = true;
            }
            break;
        case MsgType::Nack:
            if (auto n = decodeNack(body)) handleNack(*n);
            break;
        case MsgType::Ping:
            channel_->send(encodePing(MsgType::Pong, body.u64()));
            break;
        case MsgType::Bye:
            running_ = false;
            break;
        default:
            break;
        }
    }
}

void Session::run() {
    Bytes first;
    if (!channel_->recv(first) || first.empty() || first[0] != static_cast<uint8_t>(MsgType::Hello)) return;
    auto hello = decodeHello(Reader(first.data() + 1, first.size() - 1));
    if (!hello || hello->protocol != kProtocolVersion) {
        logf("rejected client: bad hello or protocol version");
        return;
    }
    hello_ = *hello;
    if (hello_.codecs == 0) hello_.codecs = kCodecH264;
    viewportW_ = hello_.nativeWidth;
    viewportH_ = hello_.nativeHeight;
    logf("client: %s %dx%d codecs=%d udp=%d", hello_.deviceName.c_str(), hello_.nativeWidth, hello_.nativeHeight,
         hello_.codecs, hello_.wantUdp ? 1 : 0);

    useUdp_ = hello_.wantUdp && !loopback_;
    // The loopback test tool asks for UDP explicitly and has no other way to get it.
    if (hello_.wantUdp && loopback_) useUdp_ = true;

    if (useUdp_) {
        udp_ = Socket::udpBind(0);
        if (!udp_.valid()) {
            logf("cannot open UDP socket");
            return;
        }
        udp_.setBuffers(4 * 1024 * 1024, 0);
        udp_.setRecvTimeout(200);
        if (!channel_->send(encode(UdpSetup{udp_.localPort()}))) return;
        udpRecvThread_ = std::thread([this] {
            Bytes buf(256), plain(256);
            while (running_) {
                uint32_t ip = 0;
                uint16_t port = 0;
                int n = udp_.recvFrom(buf.data(), buf.size(), &ip, &port);
                if (n != static_cast<int>(kUdpProbeBytes)) continue;
                uint64_t seq;
                // Only an authenticated probe may redirect the video stream.
                if (udpOpen(keys_.udpH2C, buf.data(), static_cast<size_t>(n), plain.data(), &seq) && seq >= kProbeSeqBase) {
                    clientUdpPort_ = port;
                    if (clientUdpIp_.exchange(ip) == 0) keyRequested_ = true;  // anything earlier never left
                }
            }
        });
    }

    if (host_.options().enableTouch) {
        std::lock_guard<std::mutex> lock(touchMutex_);
        touch_ = std::make_unique<TouchInjector>();
    }

    const char* transport = useUdp_ ? "wifi" : "usb";
    host_.publishStatus([&](LiveStatus& l) {
        l = LiveStatus{};
        l.connected = true;
        l.device = hello_.deviceName;
        l.transport = transport;
    });

    std::thread sender(&Session::senderLoop, this);
    std::thread video(&Session::videoLoop, this);
    recvLoop();
    requestStop();
    video.join();
    sender.join();
    if (udpRecvThread_.joinable()) udpRecvThread_.join();
    {
        std::lock_guard<std::mutex> lock(touchMutex_);
        touch_.reset();  // lifts anything still pressed
    }
    host_.publishStatus([](LiveStatus& l) {
        l = LiveStatus{};
    });
    logf("client disconnected");
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------
Host::Host(HostOptions opts) : opts_(std::move(opts)), adb_(opts_.port), live_(std::make_unique<LiveStatus>()) {}

Host::~Host() { stop(); }

void Host::publishStatus(const std::function<void(LiveStatus&)>& edit) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    edit(*live_);
}

void Host::markOutputAttached(const std::wstring& device) {
    std::lock_guard<std::mutex> lock(attachMutex_);
    attachedOutput_ = device;
    detachAtUs_ = 0;
}

void Host::detachIfIdle(bool force) {
    std::wstring device;
    {
        std::lock_guard<std::mutex> lock(attachMutex_);
        if (attachedOutput_.empty()) return;
        if (sessionActive_.load() && !force) {
            detachAtUs_ = 0;
            return;
        }
        uint64_t now = monotonicUs();
        if (!force) {
            if (detachAtUs_ == 0) {
                detachAtUs_ = now + kDetachGraceUs;  // short drops must not reshuffle windows
                return;
            }
            if (now < detachAtUs_) return;
        }
        device = attachedOutput_;
        attachedOutput_.clear();
        detachAtUs_ = 0;
    }
    detachOutput(device);
    logf("virtual display taken off the desktop");
}

bool Host::start() {
    netInit();
    store_.load();
    // Wireless can be toggled at runtime, so the check happens per connection.
    listener_ = Socket::tcpListen(opts_.port, false);
    if (!listener_.valid()) {
        logf("cannot listen on port %d", opts_.port);
        return false;
    }
    running_ = true;
    acceptThread_ = std::thread(&Host::acceptLoop, this);
    if (opts_.enableAdb) adb_.start();
    if (opts_.enableIpc) {
        ipc_ = std::make_unique<IpcServer>([this](const std::string& r) { return handleCommand(r); });
        ipc_->start();
    }
    if (opts_.enableDiscovery && store_.settings().allowWireless) discovery_.start(opts_.port);
    logf("host started on port %d", opts_.port);
    return true;
}

void Host::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (session_) session_->requestStop();
    }
    listener_.shutdownBoth();
    listener_.close();
    if (acceptThread_.joinable()) acceptThread_.join();
    if (sessionThread_.joinable()) sessionThread_.join();
    if (ipc_) ipc_->stop();
    adb_.stop();
    discovery_.stop();
    detachIfIdle(true);
}

std::optional<std::string> Host::takePairingCodeAttempt() {
    std::lock_guard<std::mutex> lock(pairMutex_);
    if (pairingCode_.empty()) return std::nullopt;
    if (monotonicUs() > pairingExpiresUs_ || pairingAttempts_ >= kPairingMaxAttempts) {
        pairingCode_.clear();
        return std::nullopt;
    }
    ++pairingAttempts_;
    return pairingCode_;
}

void Host::acceptLoop() {
    while (running_) {
        detachIfIdle(false);
        std::string peer;
        bool loopback = false;
        Socket s = listener_.accept(500, &peer, &loopback);
        if (!s.valid()) continue;

        if (!loopback && !store_.settings().allowWireless) {
            logf("refused %s: wireless is turned off", peer.c_str());
            continue;
        }
        if (sessionActive_.load()) {
            logf("refused %s: a phone is already connected", peer.c_str());
            continue;
        }
        if (sessionThread_.joinable()) sessionThread_.join();

        // A silent peer must not hold up the port.
        s.setRecvTimeout(10000);
        HostAuth auth;
        auth.peerIsLoopback = loopback;
        auth.currentPairingCode = [this] { return takePairingCodeAttempt(); };
        auth.lookupPaired = [this](const ClientId& id) -> std::optional<Key32> {
            auto d = store_.findPaired(id);
            if (!d) return std::nullopt;
            return d->secret;
        };
        auto hs = handshakeHost(s, auth);
        if (!hs) {
            logf("handshake failed from %s", peer.c_str());
            continue;
        }
        s.setRecvTimeout(0);

        auto session = std::make_shared<Session>(*this, std::move(s), *hs, loopback);
        const bool newPair = hs->mode == AuthMode::Pairing;
        if (newPair) {
            std::lock_guard<std::mutex> lock(pairMutex_);
            pairingCode_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            session_ = session;
        }
        sessionActive_ = true;
        const auto clientId = hs->clientId;
        const auto secret = hs->keys.pairSecret;
        sessionThread_ = std::thread([this, session, newPair, clientId, secret] {
            if (newPair) {
                PairedDevice d;
                d.id = clientId;
                d.secret = secret;
                d.name = "Android";
                store_.addPaired(d);
            }
            session->run();
            if (newPair && !session->deviceName().empty()) {
                PairedDevice d;
                d.id = clientId;
                d.secret = secret;
                d.name = session->deviceName();
                store_.addPaired(d);
            }
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                session_.reset();
            }
            sessionActive_ = false;
        });
    }
    // Stop wants the display gone at once.
}

std::string Host::statusJson() {
    LiveStatus l;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        l = *live_;
    }
    auto settings = store_.settings();
    std::string code;
    long long codeSeconds = 0;
    {
        std::lock_guard<std::mutex> lock(pairMutex_);
        uint64_t now = monotonicUs();
        if (!pairingCode_.empty() && now < pairingExpiresUs_ && pairingAttempts_ < kPairingMaxAttempts) {
            code = pairingCode_;
            codeSeconds = static_cast<long long>((pairingExpiresUs_ - now) / 1000000);
        }
    }

    std::ostringstream o;
    o << "{\"ok\":true"
      << ",\"connected\":" << (l.connected ? "true" : "false")
      << ",\"device\":\"" << jsonEscape(l.device) << "\""
      << ",\"transport\":\"" << l.transport << "\""
      << ",\"error\":\"" << jsonEscape(l.error) << "\""
      << ",\"encoder\":\"" << jsonEscape(l.encoder) << "\""
      << ",\"codec\":\"" << l.codec << "\""
      << ",\"displayW\":" << l.displayW << ",\"displayH\":" << l.displayH << ",\"hz\":" << l.hz
      << ",\"encodeW\":" << l.encodeW << ",\"encodeH\":" << l.encodeH << ",\"fps\":" << l.fps
      << ",\"kbps\":" << l.kbps << ",\"measuredKbps\":" << l.measuredKbps
      << ",\"rttMs\":" << l.rttMs << ",\"decodeMs\":" << l.decodeMs
      << ",\"encodeMs\":" << l.encodeMs << ",\"measuredFps\":" << l.measuredFps
      << ",\"lossPercent\":" << l.lossPercent << ",\"framesDropped\":" << l.framesDropped
      << ",\"usingVirtualDisplay\":" << (l.usingVirtualDisplay ? "true" : "false")
      << ",\"virtualDisplayInstalled\":" << (vddInstalled() ? "true" : "false")
      << ",\"adbFound\":" << (adb_.adbFound() ? "true" : "false")
      << ",\"pairingCode\":\"" << code << "\",\"pairingSeconds\":" << codeSeconds
      << ",\"settings\":{\"width\":" << settings.width << ",\"height\":" << settings.height << ",\"hz\":" << settings.hz
      << ",\"maxKbps\":" << settings.maxKbps << ",\"allowWireless\":" << (settings.allowWireless ? "true" : "false") << "}"
      << ",\"usbDevices\":[";
    bool first = true;
    for (const auto& d : adb_.devices()) {
        if (!first) o << ',';
        first = false;
        o << "{\"serial\":\"" << jsonEscape(d.serial) << "\",\"state\":\"" << jsonEscape(d.state) << "\"}";
    }
    o << "],\"pairedDevices\":[";
    first = true;
    for (const auto& d : store_.pairedList()) {
        if (!first) o << ',';
        first = false;
        o << "{\"id\":\"" << toHex(d.id.data(), d.id.size()) << "\",\"name\":\"" << jsonEscape(d.name) << "\"}";
    }
    o << "]}";
    return o.str();
}

std::string Host::handleCommand(const std::string& json) {
    const std::string cmd = jsonGetString(json, "cmd");
    if (cmd == "status") return statusJson();
    if (cmd == "set") {
        Settings s = store_.settings();
        s.width = static_cast<uint16_t>(jsonGetNumber(json, "width", s.width));
        s.height = static_cast<uint16_t>(jsonGetNumber(json, "height", s.height));
        s.hz = static_cast<uint16_t>(jsonGetNumber(json, "hz", s.hz));
        s.maxKbps = static_cast<uint32_t>(jsonGetNumber(json, "maxKbps", s.maxKbps));
        const bool wasWireless = s.allowWireless;
        s.allowWireless = jsonGetBool(json, "allowWireless", s.allowWireless);
        store_.setSettings(s);
        if (s.allowWireless != wasWireless) {
            if (s.allowWireless) discovery_.start(opts_.port);
            else discovery_.stop();
        }
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (session_) session_->settingsChanged();
        return R"({"ok":true})";
    }
    if (cmd == "pair_start") {
        std::lock_guard<std::mutex> lock(pairMutex_);
        pairingCode_ = generatePairingCode();
        pairingExpiresUs_ = monotonicUs() + kPairingLifetimeUs;
        pairingAttempts_ = 0;
        return std::string(R"({"ok":true,"code":")") + pairingCode_ + "\"}";
    }
    if (cmd == "pair_cancel") {
        std::lock_guard<std::mutex> lock(pairMutex_);
        pairingCode_.clear();
        return R"({"ok":true})";
    }
    if (cmd == "unpair") {
        bool removed = store_.removePaired(jsonGetString(json, "id"));
        return std::string(R"({"ok":)") + (removed ? "true" : "false") + "}";
    }
    if (cmd == "disconnect") {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (session_) session_->requestStop();
        return R"({"ok":true})";
    }
    return R"({"ok":false,"error":"unknown command"})";
}

}  // namespace sd
