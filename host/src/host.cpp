#include "host.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

#include "capture.h"
#include "display.h"
#include "encoder.h"
#include "log.h"
#include "vdd_device.h"

#include <shellapi.h>

namespace sd {

namespace {
constexpr size_t kMaxConnections = kMaxDevices + 4;  // including those still in the handshake
constexpr int kEncoderFailuresBeforeGivingUp = 3;
constexpr int kFramesThatProveAnEncoder = 120;
constexpr int kFramesToWaitForKeyframe = 30;
constexpr uint64_t kPairingLifetimeUs = 120'000'000;
constexpr int kPairingMaxAttempts = 5;
constexpr uint64_t kDetachGraceUs = 5'000'000;
constexpr uint64_t kIdleKeepAliveUs = 500'000;
constexpr uint64_t kPlaceCheckUs = 200'000;
constexpr int kRepeatAfterChange = 6;  // flushes decoders that hold on to a few frames
constexpr size_t kUdpProbeBytes = 4 + kUdpOverhead;
constexpr uint64_t kProbeSeqBase = 1ull << 63;

const char* codecName(uint8_t c) { return c == kCodecHevc ? "HEVC" : "H.264"; }

bool sameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

// Test tools have no identity, so there is nothing to recognise them by.
bool hasIdentity(const ClientId& id) {
    return std::any_of(id.begin(), id.end(), [](uint8_t b) { return b != 0; });
}

}  // namespace

// ---------------------------------------------------------------------------
// One connected phone.
// ---------------------------------------------------------------------------
class Session {
public:
    Session(Host& host, Socket&& sock, const HandshakeResult& hs, bool loopback)
        : host_(host), keys_(hs.keys), mode_(hs.mode), clientId_(hs.clientId), loopback_(loopback) {
        if (hasIdentity(clientId_)) {
            id_ = toHex(clientId_.data(), clientId_.size());
            settings_ = host_.store().deviceSettings(clientId_);
        } else {
            static std::atomic<uint32_t> next{1};
            id_ = "connection-" + std::to_string(next.fetch_add(1));
            copyDisplayDefaults(host_.currentSettings(), settings_);
        }
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
    // Reads what the device says first: closing with unread data could lose the message.
    void refuseFull() {
        Bytes first;
        channel_->recv(first);
        channel_->send(Bytes{static_cast<uint8_t>(MsgType::Bye), kByeHostFull});
        channel_->shutdown();
    }
    const std::string& deviceName() const { return hello_.deviceName; }
    const std::string& id() const { return id_; }

    DeviceSettings settings() const {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        return settings_;
    }
    // What the user chose for this device. Nothing happens if nothing differs.
    void displaysChanged() { displayDirty_ = true; }
    // Ends the capture on a virtual display, if there is one, and says whether none is left.
    bool stopVirtualCapture() {
        displayDirty_ = true;
        return !capturingVirtual_;
    }
    void chooseDisplay(uint16_t width, uint16_t height, uint16_t hz, uint32_t maxKbps, bool mirror, uint16_t scale, const std::string& encoder, uint8_t fill) {
        if (fill > kFillTile) fill = kFillFit;
        bool streamSame = false;
        {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            streamSame = settings_.width == width && settings_.height == height && settings_.hz == hz && settings_.maxKbps == maxKbps &&
                         settings_.mirror == mirror && settings_.scalePercent == scale && settings_.encoder == encoder;
            if (streamSame && settings_.fill == fill) return;
            settings_.fill = fill;
            settings_.width = width;
            settings_.height = height;
            settings_.hz = hz;
            settings_.maxKbps = maxKbps;
            settings_.mirror = mirror;
            settings_.scalePercent = scale;
            settings_.encoder = encoder;
            settings_.custom = true;
            saveSettings();
        }
        // How the client shows the picture is no reason to interrupt making it.
        if (streamSame) fillDirty_ = true;
        else displayDirty_ = true;
    }
    // What this device will ask for the next time it sets up its display.
    DisplayMode wantedMode() const {
        const DeviceSettings s = settings();
        const SetDisplay manual{s.width, s.height, s.hz, s.maxKbps};
        const Size pc = primaryDisplaySize();
        return chooseMode(hello_, viewportW_.load(), viewportH_.load(), manual, pc.width, pc.height);
    }
    void followDefaults(const Settings& s) {
        {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            copyDisplayDefaults(s, settings_);
            settings_.custom = false;
        }
        displayDirty_ = true;
    }
    LiveStatus live() {
        std::lock_guard<std::mutex> lock(liveMutex_);
        LiveStatus l = live_;
        l.id = id_;
        return l;
    }

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
    DisplayOptions displayOptions() const;
    void publish(const std::function<void(LiveStatus&)>& edit) {
        std::lock_guard<std::mutex> lock(liveMutex_);
        edit(live_);
    }
    void saveSettings() {  // settingsMutex_ held
        if (hasIdentity(clientId_)) host_.store().setDeviceSettings(clientId_, settings_);
    }
    void rememberPlace(const RECT& r) {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        if (settings_.left == r.left && settings_.top == r.top && settings_.right == r.right && settings_.bottom == r.bottom) return;
        settings_.left = r.left;
        settings_.top = r.top;
        settings_.right = r.right;
        settings_.bottom = r.bottom;
        saveSettings();
    }

    Host& host_;
    SessionKeys keys_;
    AuthMode mode_;
    ClientId clientId_;
    std::string id_;
    mutable std::mutex settingsMutex_;
    DeviceSettings settings_;
    std::mutex liveMutex_;
    LiveStatus live_;
    bool loopback_;
    uint32_t peerIp_ = 0;
    std::unique_ptr<SecureChannel> channel_;
    Hello hello_;

    std::atomic<bool> running_{true};
    std::atomic<bool> displayDirty_{false};
    std::atomic<bool> fillDirty_{false};
    // Becomes true only under the display lock, so it stays false for whoever holds that lock.
    std::atomic<bool> capturingVirtual_{false};
    std::atomic<bool> keyRequested_{true};
    std::atomic<uint16_t> viewportW_{0}, viewportH_{0};
    uint16_t recommendedScale_ = 100, currentScale_ = 100;  // video thread only
    bool vddPlaced_ = false;  // video thread only: this session has put the display on the desktop
    std::wstring monitor_;  // video thread only
    uint32_t nextFrameId_ = 0;  // video thread only
    std::vector<std::string> badEncoders_;  // video thread only: kinds that keep failing here
    std::string failingEncoder_;            // video thread only
    int encoderFailures_ = 0;               // video thread only

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

DisplayOptions Session::displayOptions() const {
    DisplayOptions o;
    o.phone = {hello_.nativeWidth, hello_.nativeHeight};
    o.pc = primaryDisplaySize();
    o.recommendedScale = recommendedScale_;
    o.currentScale = currentScale_;
    o.resolutions = commonResolutions(std::max(hello_.maxDecodeWidth, hello_.maxDecodeHeight),
                                      std::min(hello_.maxDecodeWidth, hello_.maxDecodeHeight));
    return o;
}

uint32_t Session::bitrateCap(const DisplayMode& m) const {
    const uint32_t chosen = settings().maxKbps;
    if (chosen) return chosen;
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
    const DeviceSettings s = settings();
    DisplayMode want = wantedMode();
    *mode = want;

    if (!opts.sourceOverride.empty() && !opts.useVirtualDisplayInTest) {
        *device = opts.sourceOverride;
        *isVirtual = false;
        return true;
    }

    auto displays = host_.lockDisplays();
    if (s.mirror) {
        // Mirroring reads the main display as it is, so nothing about it changes.
        vddPlaced_ = false;
        host_.releaseDisplay(this, true);
        for (const auto& o : listOutputs()) {
            if (o.primary) {
                *device = o.deviceName;
                *isVirtual = false;
                return true;
            }
        }
        logf("no primary display found");
        return false;
    }

    bool changedDriver = false;
    const char* problem = host_.prepareDriver(this, want, &changedDriver);
    if (problem) publish([&](LiveStatus& l) { l.error = problem; });
    if (!vddMonitorPresent()) {
        if (!problem) publish([](LiveStatus& l) { l.error = "virtual_display_missing"; });
        logf("virtual display driver not found");
        return false;
    }
    if (!host_.saveLayoutBeforeChange()) return false;
    monitor_ = host_.claimDisplay(this, clientId_);
    if (monitor_.empty()) {
        publish([](LiveStatus& l) { l.error = "virtual_display_count"; });
        return false;
    }
    const std::wstring isolated = isolateVdd(monitor_, host_.claimedDisplays());
    if (isolated.empty()) {
        publish([](LiveStatus& l) { l.error = "virtual_display_layout"; });
        return false;
    }
    auto vdd = findOutputByName(isolated);
    if (!vdd || !vdd->isVdd) {
        logf("virtual display source %s not usable yet", narrow(isolated).c_str());
        return false;
    }
    *device = vdd->deviceName;
    *isVirtual = true;
    logf("%s shows %s on %s", hello_.deviceName.c_str(), narrow(monitor_).c_str(), narrow(*device).c_str());

    // While it is showing, the user may have moved it, so where it is now counts.
    // Coming back onto the desktop, Windows picks a place, so the last known one counts.
    // Read here, because the driver reload below can put it somewhere else.
    RECT previous{};
    bool hasPrevious = false;
    if (vddPlaced_ && vdd->attached) {
        previous = vdd->rect;
        hasPrevious = true;
    } else if (s.right > s.left && s.bottom > s.top) {
        previous = {s.left, s.top, s.right, s.bottom};
        hasPrevious = true;
    }

    // Modes can take a moment to show up after the driver has restarted.
    for (int i = 0; changedDriver && i < 30 && running_ && !outputHasMode(*device, want); ++i) Sleep(100);
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
    if (!attachOutput(*device, *mode, hasPrevious ? &previous : nullptr)) return false;
    vddPlaced_ = true;
    capturingVirtual_ = true;
    if (RECT placed; outputRect(*device, &placed)) rememberPlace(placed);

    // Before capture starts: a change of scale would interrupt it.
    recommendedScale_ = recommendedScale(mode->width, mode->height, hello_.densityDpi);
    const uint16_t wantScale = s.scalePercent ? s.scalePercent : recommendedScale_;
    // Just after the driver has restarted, this can fail for a moment.
    bool scaleSet = setVddScale(monitor_, wantScale);
    for (int i = 0; !scaleSet && changedDriver && i < 10 && running_; ++i) {
        Sleep(100);
        scaleSet = setVddScale(monitor_, wantScale);
    }
    if (!scaleSet) logf("could not set display scale to %d%%", wantScale);
    currentScale_ = getVddScale(monitor_);
    logf("display scale %d%% (wanted %d%%, recommended %d%%)", currentScale_, wantScale, recommendedScale_);

    // After the driver has restarted, Windows goes on rearranging and can take a monitor off the
    // desktop again. Going on would capture whichever monitor gets that source next: a device
    // showing another device's picture, while that one cannot capture anything.
    if (vddMonitorSource(monitor_) != *device) {
        logf("%s lost its place on the desktop; setting it up again", hello_.deviceName.c_str());
        capturingVirtual_ = false;
        return false;
    }
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
        capturingVirtual_ = false;  // the capture of the last round is closed by now

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
            if (capture.open(device, !isVirtual)) { opened = true; break; }
            Sleep(100);
        }
        if (!opened) {
            if (++openFailures % 5 == 1) logf("cannot capture %s", narrow(device).c_str());
            Sleep(500);
            continue;
        }
        openFailures = 0;

        const int capW = capture.width(), capH = capture.height();
        RECT shownRect{};
        if (auto o = findOutputByName(device)) {
            shownRect = o->rect;
            std::lock_guard<std::mutex> lock(touchMutex_);
            if (touch_) touch_->setDisplayRect(shownRect);
        }
        uint64_t lastPlaceCheckUs = monotonicUs();

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
        // Mirroring can mean a desktop larger than the phone's decoder accepts.
        double fit = 1.0;
        {
            const int longer = std::max(capW, capH), shorter = std::min(capW, capH);
            const int maxLong = std::max(hello_.maxDecodeWidth, hello_.maxDecodeHeight);
            const int maxShort = std::min(hello_.maxDecodeWidth, hello_.maxDecodeHeight);
            if (maxLong > 0 && maxShort > 0)
                fit = std::min({1.0, static_cast<double>(maxLong) / longer, static_cast<double>(maxShort) / shorter});
        }
        const int encW = std::max(320, static_cast<int>(capW * fit) * scale / 100) & ~1;
        const int encH = std::max(240, static_cast<int>(capH * fit) * scale / 100) & ~1;
        const int fps = startTarget.fps;

        uint8_t codec = (hello_.codecs & kCodecHevc) ? kCodecHevc : kCodecH264;

        // Same adapter first: no copy through system memory. Then any other adapter.
        ComPtr<ID3D11Device> encDev = capture.device();
        ComPtr<ID3D11DeviceContext> encCtx = capture.context();
        std::unique_ptr<Encoder> encoder;
        // A GPU's own interface first, as it does what is asked of it most directly. Media
        // Foundation is what remains for the GPUs and drivers those do not cover.
        using Create = std::unique_ptr<Encoder> (*)(const EncoderConfig&);
        static const std::pair<const char*, Create> kHardwareEncoders[] = {
            {"nvenc", createNvencEncoder}, {"amf", createAmfEncoder}, {"qsv", createQsvEncoder}, {"mf", createMfEncoder}};
        const auto isBad = [&](const char* kind) { return std::find(badEncoders_.begin(), badEncoders_.end(), kind) != badEncoders_.end(); };
        auto tryOn = [&](ID3D11Device* dev, const std::string& onlyKind = {}) {
            if (onlyKind == "sw") return false;
            // WARP has nothing to encode with, and some drivers do not expect to be asked.
            if (adapterVendor(dev) == kVendorMicrosoft) return false;
            EncoderConfig ec;
            ec.device = dev;
            ec.width = encW;
            ec.height = encH;
            ec.fps = fps;
            ec.kbps = startTarget.bitrateKbps;
            for (uint8_t c : {codec, static_cast<uint8_t>(kCodecH264)}) {
                if (!(hello_.codecs & c)) continue;
                ec.codec = c;
                for (const auto& [kind, create] : kHardwareEncoders) {
                    if ((!onlyKind.empty() && onlyKind != kind) || isBad(kind)) continue;
                    encoder = create(ec);
                    if (encoder) { codec = c; return true; }
                }
            }
            return false;
        };
        bool crossAdapter = false;

        // What the user chose comes first. If that does not work, the stream still should.
        const std::vector<GpuInfo> gpus = listGpus();
        const auto gpuNameOf = [&](const LUID& luid) {
            for (const auto& g : gpus)
                if (sameLuid(g.luid, luid)) return narrow(g.name);
            return std::string{};
        };
        std::string captureGpu = gpuNameOf(capture.adapterLuid()), encoderGpu = captureGpu;
        const std::string chosen = settings().encoder;
        bool encoderFallback = false;
        // Software first among codecs in H.264, which takes far less of a CPU than HEVC does.
        const auto trySoftware = [&] {
            if (isBad("sw")) return false;
            EncoderConfig ec;
            ec.width = encW;
            ec.height = encH;
            ec.fps = fps;
            ec.kbps = startTarget.bitrateKbps;
            for (uint8_t c : {static_cast<uint8_t>(kCodecH264), static_cast<uint8_t>(kCodecHevc)}) {
                if (!(hello_.codecs & c)) continue;
                ec.codec = c;
                encoder = createSoftwareEncoder(ec);
                if (encoder) {
                    codec = c;
                    encoderGpu = "CPU";
                    return true;
                }
            }
            return false;
        };
        if (const auto bar = chosen.find('|'); bar != std::string::npos) {
            const std::string kind = chosen.substr(0, bar), gpu = chosen.substr(bar + 1);
            if (kind == "sw") {
                trySoftware();
            } else if (gpu == captureGpu) {
                tryOn(capture.device(), kind);
            } else {
                for (const auto& g : gpus) {
                    if (narrow(g.name) != gpu) continue;
                    ComPtr<ID3D11DeviceContext> ctx;
                    auto dev = createDeviceOnAdapter(g.luid, &ctx);
                    if (dev && tryOn(dev.Get(), kind)) {
                        encDev = dev;
                        encCtx = ctx;
                        crossAdapter = true;
                        encoderGpu = gpu;
                    }
                    break;
                }
            }
            if (!encoder) {
                logf("the chosen encoder %s is not available; choosing automatically", chosen.c_str());
                encoderFallback = true;
            }
        }
        if (!encoder && !tryOn(capture.device())) {
            for (const auto& [luid, vendor] : listHardwareAdapters()) {
                if (sameLuid(luid, capture.adapterLuid())) continue;
                ComPtr<ID3D11DeviceContext> ctx;
                auto dev = createDeviceOnAdapter(luid, &ctx);
                if (dev && tryOn(dev.Get())) {
                    encDev = dev;
                    encCtx = ctx;
                    crossAdapter = true;
                    encoderGpu = gpuNameOf(luid);
                    break;
                }
            }
        }
        // Better a slow picture than none: a virtual machine has no hardware encoder at all.
        if (!encoder && trySoftware()) logf("no hardware encoder available: encoding on the CPU");
        if (!encoder) {
            logf("no encoder available");
            publish([](LiveStatus& l) { l.error = "no_encoder"; });
            Sleep(2000);
            continue;
        }

        const bool toMemory = encoder->wantsMemoryInput();
        CrossAdapterCopy cross;
        if (crossAdapter && !cross.init(capture.device(), capture.context(), encDev.Get(), encCtx.Get(), capW, capH)) {
            logf("cross-adapter copy init failed");
            Sleep(1000);
            continue;
        }
        Nv12Converter conv;
        CpuNv12Converter cpuConv;
        const bool converterReady = toMemory ? cpuConv.init(capture.device(), capture.context(), capW, capH, encW, encH)
                                             : conv.init(encDev.Get(), encCtx.Get(), capW, capH, encW, encH);
        if (!converterReady) {
            logf("colour converter init failed");
            Sleep(1000);
            continue;
        }
        const bool paceByOutput = encoder->overshootsTarget();
        uint64_t nextEncodeAllowedUs = 0;
        int framesFromThisEncoder = 0;
        int framesSinceKeyRequest = -1;  // not waiting for one
        const bool breakThisEncoder = host_.options().testBreakEncoder.find(encoder->kind()) != std::string::npos;
        const bool deafThisEncoder = host_.options().testDeafEncoder == encoder->kind();

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
        {
            const DeviceSettings s = settings();
            cfg.mirror = s.mirror;
            cfg.manualWidth = s.width;
            cfg.manualHeight = s.height;
            cfg.manualHz = s.hz;
            cfg.manualScale = s.scalePercent;
            cfg.fill = s.fill;
        }
        fillDirty_ = false;
        if (!channel_->send(encode(cfg)) || !channel_->send(encode(displayOptions()))) { requestStop(); return; }
        keyRequested_ = true;

        publish([&](LiveStatus& l) {
            l.error.clear();
            l.displayW = capW;
            l.displayH = capH;
            l.hz = mode.hz;
            l.encodeW = encW;
            l.encodeH = encH;
            l.fps = fps;
            l.encoder = encoder->name();
            l.captureGpu = captureGpu;
            l.encoderGpu = encoderGpu;
            l.crossAdapter = crossAdapter;
            l.encoderFallback = encoderFallback;
            l.codec = codecName(codec);
            l.kbps = startTarget.bitrateKbps;
            l.usingVirtualDisplay = isVirtual;
            l.sourceName = narrow(device);
            l.recommendedScale = recommendedScale_;
            l.currentScale = currentScale_;
        });

        const uint64_t frameIntervalUs = 1'000'000 / std::max(fps, 1);
        uint64_t lastEncodeUs = 0, lastStatsUs = monotonicUs();
        bool unsent = false;
        int repeats = 0;
        uint64_t encodeSumUs = 0, encodeCount = 0;
        uint32_t appliedKbps = startTarget.bitrateKbps;
        uint32_t patternTick = 0;

        while (running_ && !displayDirty_) {
            if (fillDirty_.exchange(false)) {
                cfg.fill = settings().fill;
                cfg.bitrateKbps = kbpsNow_.load();
                if (!channel_->send(encode(cfg))) { requestStop(); return; }
                keyRequested_ = true;  // a client may have a new surface to show it on
            }
            if (targetDirty_.exchange(false)) {
                RateTarget t;
                {
                    std::lock_guard<std::mutex> lock(rateMutex_);
                    t = target_;
                }
                if (t.scalePercent != scale || t.fps != fps) break;  // rebuild
                if (t.bitrateKbps != appliedKbps) {
                    // Without this the stream stays too large for the path. A new encoder
                    // takes the new rate, at the cost of one keyframe.
                    if (!encoder->setBitrate(t.bitrateKbps)) {
                        logf("%s did not take %u kbps; rebuilding", encoder->name(), t.bitrateKbps);
                        break;
                    }
                    appliedKbps = t.bitrateKbps;
                    kbpsNow_ = appliedKbps;
                    publish([&](LiveStatus& l) { l.kbps = appliedKbps; });
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

            // Moving a display usually interrupts capture, which rebuilds all of this,
            // but nothing promises that. Touch must not depend on it.
            if (now - lastPlaceCheckUs >= kPlaceCheckUs) {
                lastPlaceCheckUs = now;
                // The same source can come to show another monitor, with no sign from the capture.
                if (isVirtual && vddMonitorSource(monitor_) != device) {
                    logf("%s is no longer on %s; rebuilding", hello_.deviceName.c_str(), narrow(device).c_str());
                    break;
                }
                RECT r;
                if (outputRect(device, &r) && !EqualRect(&r, &shownRect)) {
                    logf("display moved to %ld,%ld", r.left, r.top);
                    shownRect = r;
                    {
                        std::lock_guard<std::mutex> lock(touchMutex_);
                        if (touch_) touch_->setDisplayRect(r);
                    }
                    if (isVirtual) rememberPlace(r);
                }
            }

            const bool dueByRate = now - lastEncodeUs >= frameIntervalUs * 9 / 10;
            const bool wantRepeat = repeats > 0 && dueByRate;
            const bool keepAlive = now - lastEncodeUs >= kIdleKeepAliveUs;
            // Fewer frames rather than more data than the path takes. A request for a keyframe
            // waits as well: it is the largest kind, and what overran the path in the first place.
            const bool heldBack = paceByOutput && now < nextEncodeAllowedUs;
            if (!heldBack && (keyRequested_.load() || (unsent && dueByRate) || wantRepeat || keepAlive)) {
                ID3D11Texture2D* tex = capture.texture();
                if (crossAdapter) {
                    tex = cross.copy(tex);
                    if (!tex) break;
                }
                if (toMemory ? !cpuConv.convert(tex) : !conv.convert(tex)) {
                    logf("colour conversion failed");
                    break;
                }
                const bool force = keyRequested_.exchange(false);
                if (force && framesSinceKeyRequest < 0) framesSinceKeyRequest = 0;
                const uint64_t captureUs = now;
                const Encoder::Output deliver = [&](const uint8_t* data, size_t size, bool key) {
                    if (key) framesSinceKeyRequest = -1;
                    if (paceByOutput) nextEncodeAllowedUs = now + static_cast<uint64_t>(size) * 8000 / std::max<uint32_t>(kbpsNow_.load(), 1);
                    FrameHeader fh;
                    fh.captureUs = captureUs;
                    fh.keyframe = key;
                    fh.frameId = nextFrameId_++;
                    enqueue(encodeFrame(fh, data, size), key);
                };
                const bool passOn = force && !deafThisEncoder;
                bool encoded = toMemory ? encoder->encodeMemory(cpuConv.data(), passOn, deliver) : encoder->encode(conv.output(), passOn, deliver);
                if (breakThisEncoder && framesFromThisEncoder >= 3) encoded = false;
                if (!encoded) {
                    // Failing right from the start, again and again, says this one does not
                    // work here. The next round then goes on to the next encoder.
                    const std::string kind = encoder->kind();
                    if (framesFromThisEncoder < kFramesThatProveAnEncoder) {
                        encoderFailures_ = kind == failingEncoder_ ? encoderFailures_ + 1 : 1;
                        failingEncoder_ = kind;
                        if (encoderFailures_ >= kEncoderFailuresBeforeGivingUp) {
                            logf("%s keeps failing; not using it for this device any more", encoder->name());
                            badEncoders_.push_back(kind);
                            encoderFailures_ = 0;
                        }
                    }
                    logf("encode failed; rebuilding");
                    Sleep(100);
                    break;
                }
                ++framesFromThisEncoder;
                // A phone that lost its place cannot show anything until a keyframe comes. An
                // encoder that ignores the request is replaced by one that starts on a keyframe.
                if (framesSinceKeyRequest >= 0 && ++framesSinceKeyRequest > kFramesToWaitForKeyframe) {
                    logf("%s ignored a request for a keyframe; rebuilding", encoder->name());
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
                publish([&](LiveStatus& l) {
                    l.encodeMs = avgMs;
                    l.measuredFps = measured;
                });
                encodeSumUs = 0;
                encodeCount = 0;
                lastStatsUs = now;
            }
        }
    }
    capturingVirtual_ = false;
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
                RateTarget t, previous;
                double queueUs = 0;
                {
                    std::lock_guard<std::mutex> lock(rateMutex_);
                    previous = target_;
                    if (rate_) {
                        t = rate_->update(*s, backlog);
                        changed = !(t == target_);
                        target_ = t;
                        queueUs = rate_->queueDelayUs();
                    }
                }
                if (changed) targetDirty_ = true;
                uint32_t total = s->packetsReceived + s->packetsLost;
                const double loss = total ? 100.0 * s->packetsLost / total : 0;
                if (changed && (t.bitrateKbps < previous.bitrateKbps || t.fps != previous.fps || t.scalePercent != previous.scalePercent)) {
                    logf("rate: %s %u->%u kbps %d->%d fps scale %d->%d; queue=%.2fms gradient=%.2fms loss=%.1f%% dropped=%u backlog=%.2fms decode=%.2fms rtt=%.2fms",
                         hello_.deviceName.c_str(), previous.bitrateKbps, t.bitrateKbps, previous.fps, t.fps,
                         previous.scalePercent, t.scalePercent, queueUs / 1000.0, s->delayGradientUs / 1000.0,
                         loss, s->framesDropped, backlog, s->decodeUs / 1000.0, s->rttUs / 1000.0);
                }
                const double kbpsSeen = s->windowMs ? s->bytes * 8.0 / s->windowMs : 0;
                publish([&](LiveStatus& l) {
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
                const DeviceSettings s = settings();
                chooseDisplay(d->width, d->height, d->hz, d->maxBitrateKbps != kKeepBitrate ? d->maxBitrateKbps : s.maxKbps,
                              d->mirror != kMirrorKeep ? d->mirror == kMirrorOn : s.mirror, d->scale != kKeepScale ? d->scale : s.scalePercent, s.encoder,
                              d->fill != kFillKeep ? d->fill : s.fill);
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
    publish([&](LiveStatus& l) {
        l = LiveStatus{};
        l.connected = true;
        l.device = hello_.deviceName;
        l.phoneW = hello_.nativeWidth;
        l.phoneH = hello_.nativeHeight;
        l.maxDecodeLong = std::max(hello_.maxDecodeWidth, hello_.maxDecodeHeight);
        l.maxDecodeShort = std::min(hello_.maxDecodeWidth, hello_.maxDecodeHeight);
        l.phoneRates = hello_.refreshRates;
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
    host_.releaseDisplay(this, false);
    logf("client disconnected: %s", hello_.deviceName.c_str());
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------
Host::Host(HostOptions opts) : opts_(std::move(opts)), adb_(opts_.port) {}

Host::~Host() { stop(); }

void Host::setHostError(const std::string& error, bool onlyIfSame) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (onlyIfSame && hostError_ != error) return;
    hostError_ = onlyIfSame ? std::string{} : error;
}

static std::wstring layoutFilePath() { return dataDir() + L"\\display-layout-to-restore.bin"; }

bool Host::saveLayoutBeforeChange() {
    auto lock = lockDisplays();
    if (layoutSaved_) return true;
    DisplayLayout layout;
    // A user who runs the driver on their own keeps it on the desktop afterwards.
    if (!(ownsDriver_ ? queryLayoutWithoutVdd(layout) : queryActiveLayout(layout))) return false;
    if (layout.paths.empty()) {
        logf("no display besides the virtual one is active; not changing the layout");
        return false;
    }
    savedLayout_ = layout;
    layoutSaved_ = true;
    // On disk too: a crash or power loss must not leave the desktop rearranged.
    const std::string data = serializeLayout(layout);
    FILE* f = _wfopen(layoutFilePath().c_str(), L"wb");
    if (f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }
    return true;
}

void Host::restoreLayoutNow() {
    auto lock = lockDisplays();
    if (!layoutSaved_) return;
    if (applyLayout(savedLayout_, ownsDriver_)) logf("display layout restored");
    layoutSaved_ = false;
    DeleteFileW(layoutFilePath().c_str());
}

void Host::lastDisplayGone() {
    restoreLayoutNow();
    if (!driverOn_) return;
    driverOn_ = false;
    if (const auto error = askDisplayService("disable")) logf("cannot turn the virtual display off: %lu", *error);
}

bool Host::stopVirtualCaptures(const Session* self) {
    std::vector<std::shared_ptr<Session>> others;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        for (auto& c : connections_)
            if (c->session && c->session.get() != self) others.push_back(c->session);
    }
    for (int i = 0; i < 250; ++i) {
        bool allStopped = true;
        for (auto& s : others) allStopped = s->stopVirtualCapture() && allStopped;
        if (allStopped) return true;
        Sleep(20);
    }
    logf("a capture did not stop in time");
    return false;
}

const char* Host::prepareDriver(const Session* session, const DisplayMode& want, bool* changedDriver) {
    auto lock = lockDisplays();
    *changedDriver = false;
    ownsDriver_ = vddDriverIsOurs();  // the driver can have been installed since the start

    std::vector<DisplayMode> keep;
    bool known = false;
    for (auto& [s, m] : modesInUse_) {
        if (s == session) {
            m = want;
            known = true;
        } else {
            keep.push_back(m);
            keep.push_back({m.height, m.width, m.hz});
        }
    }
    if (!known) modesInUse_.emplace_back(session, want);

    // One monitor per device, counting those still on their way here: every restart interrupts
    // all of them, so devices arriving together must not each cause one. A device on its way
    // out leaves a monitor that can be taken over.
    const bool hasClaim = std::any_of(claims_.begin(), claims_.end(), [&](const auto& c) { return c.session == session; });
    const auto countNeeded = [&] {
        size_t needed = hasClaim ? 0 : 1;
        for (const auto& c : claims_)
            if (c.session) ++needed;
        std::lock_guard<std::mutex> lock2(sessionMutex_);
        size_t wanting = 0;
        for (auto& c : connections_) {
            // Mirroring needs no monitor. Before the session exists, assume it will want one.
            if (c->holdsPlace && !(c->session && c->session->settings().mirror)) ++wanting;
        }
        return std::min(std::max(needed, wanting), kMaxDevices);
    };
    // While on, it keeps those it has: taking one away would interrupt everyone for nothing.
    const size_t present = driverOn_ ? listVddMonitors().size() : 0;
    size_t needed = countNeeded();
    // About to change it. Devices that connect at the same moment are a few milliseconds behind.
    if (ownsDriver_ && (!driverOn_ || needed > present)) {
        Sleep(250);
        needed = countNeeded();
    }
    const size_t monitors = std::max(needed, present);

    // Both ways round: turning the phone must not take another restart.
    std::vector<DisplayMode> modes{want, {want.height, want.width, want.hz}};
    // Every restart interrupts everyone, so what the others are about to ask for goes in this one.
    {
        std::lock_guard<std::mutex> lock2(sessionMutex_);
        for (auto& c : connections_) {
            if (!c->session || c->session.get() == session || c->session->settings().mirror) continue;
            const DisplayMode m = c->session->wantedMode();
            modes.push_back(m);
            modes.push_back({m.height, m.width, m.hz});
        }
    }
    // A driver the user runs for themselves keeps the number of monitors they gave it.
    bool changed = false;
    const char* problem = nullptr;
    std::string gpu;
    if (ownsDriver_) {
        // A GPU that has gone since it was chosen is no reason to lose the display.
        const std::string chosen = store_.settings().vddGpu;
        const auto all = listGpus();
        const bool exists = std::any_of(all.begin(), all.end(), [&](const auto& g) { return narrow(g.name) == chosen; });
        gpu = exists ? chosen : "default";
    }
    if (!ensureVddConfig(modes, keep, ownsDriver_ ? static_cast<int>(monitors) : 0, gpu, &changed)) problem = "virtual_display_config_denied";

    const char* word = ownsDriver_ && !driverOn_ ? "enable" : changed ? "restart" : nullptr;
    if (!word) return problem;
    logf("%s the driver for %dx%d@%d and %zu monitors", word, want.width, want.height, want.hz, monitors);
    // Restarting takes every capture out of the way first. It can still be put off for a moment,
    // while a capture lets go, and for as long as another program holds on to the display.
    const bool restarting = strcmp(word, "restart") == 0;
    if (restarting) stopVirtualCaptures(session);
    for (int attempt = 0;; ++attempt) {
        const auto error = askDisplayService(word);
        if (!error) break;
        if (*error != ERROR_SUCCESS_RESTART_REQUIRED) {
            logf("the display service could not %s the driver: %lu", word, *error);
            return "display_service_failed";
        }
        if (attempt == 5) {
            logf("something else keeps the virtual display in use; it cannot %s", word);
            return "virtual_display_busy";
        }
        Sleep(500);
    }
    if (ownsDriver_) driverOn_ = true;
    *changedDriver = true;
    // Monitors arrive some seconds later.
    const size_t expected = ownsDriver_ ? monitors : 1;
    for (int i = 0; i < 150 && listVddMonitors().size() < expected; ++i) Sleep(100);
    if (listVddMonitors().size() < expected) logf("the driver has %zu of %zu monitors", listVddMonitors().size(), expected);
    Sleep(1000);  // Windows goes on rearranging for a moment after they appear
    return problem;
}

std::wstring Host::claimDisplay(const Session* session, const ClientId& id) {
    auto lock = lockDisplays();
    const auto present = [](const std::wstring& m) {
        const auto all = listVddMonitors();
        return std::find(all.begin(), all.end(), m) != all.end();
    };
    for (const auto& c : claims_) {
        if (c.session != session) continue;
        // Absent means the driver is reloading, for this device or another.
        for (int i = 0; i < 50 && !present(c.monitor); ++i) Sleep(100);
        return present(c.monitor) ? c.monitor : std::wstring{};
    }
    const auto take = [&](DisplayClaim& c) {
        c.session = session;
        c.owner = id;
        c.releaseAtUs = 0;
        return c.monitor;
    };
    if (hasIdentity(id)) {
        for (auto& c : claims_)
            if (!c.session && c.owner == id) return take(c);
    }
    const auto taken = [&](const std::wstring& m) {
        return std::any_of(claims_.begin(), claims_.end(), [&](const auto& c) { return c.monitor == m; });
    };
    auto monitors = listVddMonitors();
    for (const auto& m : monitors) {
        if (taken(m)) continue;
        claims_.push_back({m});
        return take(claims_.back());
    }
    // One whose device left moments ago is already on the desktop.
    for (auto& c : claims_)
        if (!c.session) return take(c);

    logf("no free virtual monitor among %zu", monitors.size());
    return {};
}

std::vector<std::wstring> Host::claimedDisplays() {
    auto lock = lockDisplays();
    std::vector<std::wstring> out;
    for (const auto& c : claims_) out.push_back(c.monitor);
    return out;
}

void Host::dropClaim(size_t index) {
    const std::wstring monitor = claims_[index].monitor;
    claims_.erase(claims_.begin() + index);
    if (claims_.empty()) lastDisplayGone();
    // A display the user had on the desktop before we came stays there.
    else if (!(layoutSaved_ && layoutHasMonitor(savedLayout_, monitor))) removeVddMonitor(monitor);
}

void Host::releaseDisplay(const Session* session, bool now) {
    auto lock = lockDisplays();
    modesInUse_.erase(std::remove_if(modesInUse_.begin(), modesInUse_.end(), [&](const auto& u) { return u.first == session; }), modesInUse_.end());
    for (size_t i = 0; i < claims_.size(); ++i) {
        if (claims_[i].session != session) continue;
        claims_[i].session = nullptr;
        if (now) dropClaim(i);
        else claims_[i].releaseAtUs = monotonicUs() + kDetachGraceUs;  // short drops must not reshuffle windows
        return;
    }
    // Mirroring from the start, or the last device moved on to mirroring.
    if (claims_.empty() && now) lastDisplayGone();
}

void Host::recoverLayoutAtStart() {
    if (!opts_.sourceOverride.empty() && !opts_.useVirtualDisplayInTest) return;
    FILE* f = _wfopen(layoutFilePath().c_str(), L"rb");
    if (f) {
        std::string data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
        DisplayLayout layout;
        if (deserializeLayout(data, layout)) {
            logf("restoring the display layout left by an earlier run");
            applyLayout(layout, ownsDriver_);
        }
        DeleteFileW(layoutFilePath().c_str());
    }
    // Nothing is connected yet, and an earlier run may not have got as far as turning it off.
    if (ownsDriver_) askDisplayService("disable");
}

void Host::detachIfIdle(bool force) {
    // This runs in the accept loop. Display work can take seconds, and a device arriving
    // meanwhile must not have to wait for that, so a busy lock just means next time.
    std::unique_lock<std::recursive_mutex> lock(displayMutex_, std::defer_lock);
    if (force) lock.lock();
    else if (!lock.try_lock()) return;
    if (force) {
        claims_.clear();
        lastDisplayGone();
        return;
    }
    const uint64_t now = monotonicUs();
    for (size_t i = claims_.size(); i-- > 0;) {
        if (!claims_[i].session && now >= claims_[i].releaseAtUs) dropClaim(i);
    }
}

bool Host::start() {
    netInit();
    store_.load();
    // Wireless can be toggled at runtime, so the check happens per connection.
    ownsDriver_ = vddDriverIsOurs();
    recoverLayoutAtStart();
    running_ = true;
    acceptThread_ = std::thread(&Host::acceptLoop, this);
    if (opts_.enableAdb) adb_.start();
    if (opts_.enableIpc) {
        // Test runs on other ports must not collide with an installed host.
        const std::wstring pipe = opts_.port == kDefaultPort ? L"SideDroidHost" : L"SideDroidHost-" + std::to_wstring(opts_.port);
        ipc_ = std::make_unique<IpcServer>(pipe, [this](const std::string& r) { return handleCommand(r); });
        ipc_->start();
    }
    if (opts_.enableDiscovery && store_.settings().allowWireless) discovery_.start(opts_.port);
    // Test runs on other ports never touch the real installation.
    if (opts_.port == kDefaultPort || !opts_.updateTestBase.empty()) {
        Updater::Config config;
        config.currentVersion = SIDEDROID_VERSION;
        if (opts_.updateTestBase.empty()) {
            config.manifestUrl = L"https://raw.githubusercontent.com/Ryorin1337/SideDroid/main/update.json";
            config.releaseBase = L"https://github.com/Ryorin1337/SideDroid/releases/download";
        } else {
            config.manifestUrl = opts_.updateTestBase + L"/update.json";
            config.releaseBase = opts_.updateTestBase;
            config.launchInstaller = false;
        }
        updater_ = std::make_unique<Updater>(std::move(config));
        updater_->start();
    }
    logf("host %s started on port %d", SIDEDROID_VERSION, opts_.port);
    return true;
}

void Host::stop() {
    if (!running_.exchange(false)) return;
    if (updater_) updater_->stop();
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        for (auto& c : connections_)
            if (c->session) c->session->requestStop();
    }
    // The accept loop wakes within half a second, and only then is the socket free to close.
    if (acceptThread_.joinable()) acceptThread_.join();
    listener_.close();
    {
        std::list<std::unique_ptr<Connection>> all;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            // One may have finished its handshake in the meantime.
            for (auto& c : connections_)
                if (c->session) c->session->requestStop();
            all.swap(connections_);
        }
        for (auto& c : all)
            if (c->thread.joinable()) c->thread.join();
    }
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

bool Host::ensureListening() {
    if (listener_.valid()) return true;
    listener_ = Socket::tcpListen(opts_.port, false);
    if (listener_.valid()) {
        if (listenFailures_ > 0) {
            logf("listening on port %d", opts_.port);
            setHostError("port_in_use", true);
        }
        listenFailures_ = 0;
        return true;
    }
    if (listenFailures_++ == 0) {
        logf("cannot listen on port %d", opts_.port);
        setHostError("port_in_use");
    }
    // Earlier versions let the adb server inherit the listening socket, which then
    // holds the port with no host behind it. Stopping the server releases it, and
    // adb starts a new one the next time it is needed.
    if (listenFailures_ == 3 && opts_.enableAdb) {
        logf("port still busy; restarting the adb server");
        adb_.killServer();
    }
    return false;
}

void Host::acceptLoop() {
    while (running_) {
        detachIfIdle(false);
        if (!ensureListening()) {
            Sleep(1000);
            continue;
        }
        std::string peer;
        bool loopback = false;
        Socket s = listener_.accept(500, &peer, &loopback);
        if (!s.valid()) continue;

        if (!loopback && !store_.settings().allowWireless) {
            logf("refused %s: wireless is turned off", peer.c_str());
            continue;
        }
        reapConnections();
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (connections_.size() >= kMaxConnections) {
            logf("refused %s: too many connections", peer.c_str());
            continue;
        }
        connections_.push_back(std::make_unique<Connection>());
        Connection* conn = connections_.back().get();
        // On its own thread: a slow handshake must not keep the next device waiting.
        conn->thread = std::thread(&Host::serveConnection, this, std::move(s), std::move(peer), loopback, conn);
    }
    // Stop wants the display gone at once.
}

void Host::reapConnections() {
    std::list<std::unique_ptr<Connection>> finished;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if ((*it)->done) finished.splice(finished.end(), connections_, it++);
            else ++it;
        }
    }
    for (auto& c : finished)
        if (c->thread.joinable()) c->thread.join();
}

bool Host::takePlace(Connection* conn, const ClientId& id) {
    std::unique_lock<std::mutex> lock(sessionMutex_);
    // The same device again: its old connection is usually one that died unnoticed.
    if (hasIdentity(id)) {
        const auto holders = [&] {
            std::vector<Connection*> out;
            for (auto& c : connections_)
                if (c->holdsPlace && c->clientId == id) out.push_back(c.get());
            return out;
        };
        const uint64_t deadline = monotonicUs() + 10'000'000;
        for (auto old = holders(); !old.empty(); old = holders()) {
            if (monotonicUs() > deadline) return false;
            for (auto* c : old)
                if (c->session) c->session->requestStop();
            sessionCv_.wait_for(lock, std::chrono::milliseconds(100));
        }
    }
    const auto holding = std::count_if(connections_.begin(), connections_.end(), [](const auto& c) { return c->holdsPlace; });
    if (holding >= static_cast<std::ptrdiff_t>(kMaxDevices)) return false;
    conn->holdsPlace = true;
    conn->clientId = id;
    return true;
}

void Host::serveConnection(Socket s, std::string peer, bool loopback, Connection* conn) {
    const auto finish = [&] {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        conn->session.reset();
        conn->holdsPlace = false;
        conn->done = true;
        sessionCv_.notify_all();
    };

    // A silent peer must not hold up its place for long.
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
        finish();
        return;
    }

    // Saved before anything can turn the device away: it has saved its half already.
    const bool newPair = hs->mode == AuthMode::Pairing;
    if (newPair) {
        {
            std::lock_guard<std::mutex> lock(pairMutex_);
            pairingCode_.clear();
        }
        PairedDevice d;
        d.id = hs->clientId;
        d.secret = hs->keys.pairSecret;
        d.name = "Android";
        store_.addPaired(d);
    }

    if (!takePlace(conn, hs->clientId)) {
        logf("refused %s: %zu devices are already connected", peer.c_str(), kMaxDevices);
        Session(*this, std::move(s), *hs, loopback).refuseFull();
        finish();
        return;
    }
    s.setRecvTimeout(0);
    auto session = std::make_shared<Session>(*this, std::move(s), *hs, loopback);
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        conn->session = session;
    }
    if (running_) session->run();
    if (newPair && !session->deviceName().empty()) {
        PairedDevice d;
        d.id = hs->clientId;
        d.secret = hs->keys.pairSecret;
        d.name = session->deviceName();
        store_.addPaired(d);
    }
    finish();
}

static std::string settingsJson(uint16_t width, uint16_t height, uint16_t hz, uint32_t maxKbps, bool mirror, uint16_t scale, const std::string& encoder, uint8_t fill) {
    std::ostringstream o;
    o << "\"width\":" << width << ",\"height\":" << height << ",\"hz\":" << hz << ",\"maxKbps\":" << maxKbps
      << ",\"mirror\":" << (mirror ? "true" : "false") << ",\"scale\":" << scale << ",\"encoder\":\"" << jsonEscape(encoder) << "\",\"fill\":" << static_cast<int>(fill);
    return o.str();
}

static std::string optionsJson(const LiveStatus& l, const Size& pc) {
    std::ostringstream o;
    o << "{\"phoneW\":" << l.phoneW << ",\"phoneH\":" << l.phoneH << ",\"pcW\":" << pc.width << ",\"pcH\":" << pc.height
      << ",\"recommendedScale\":" << l.recommendedScale << ",\"currentScale\":" << l.currentScale << ",\"rates\":[";
    bool first = true;
    for (auto r : l.phoneRates) {
        if (!first) o << ',';
        first = false;
        o << r;
    }
    o << "],\"resolutions\":[";
    first = true;
    for (const auto& r : commonResolutions(l.maxDecodeLong, l.maxDecodeShort)) {
        if (!first) o << ',';
        first = false;
        o << "{\"w\":" << r.width << ",\"h\":" << r.height << "}";
    }
    o << "]}";
    return o.str();
}

std::string Host::statusJson() {
    std::string hostError;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        hostError = hostError_;
    }
    std::vector<std::pair<LiveStatus, DeviceSettings>> devices;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        for (auto& c : connections_)
            if (c->session) devices.emplace_back(c->session->live(), c->session->settings());
    }
    auto settings = store_.settings();
    const Size pc = primaryDisplaySize();
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
      << ",\"connected\":" << (devices.empty() ? "false" : "true")
      << ",\"maxDevices\":" << kMaxDevices
      << ",\"error\":\"" << jsonEscape(hostError) << "\""
      << ",\"devices\":[";
    {
        bool firstDevice = true;
        for (const auto& [l, d] : devices) {
            if (!firstDevice) o << ',';
            firstDevice = false;
            o << "{\"id\":\"" << l.id << "\""
              << ",\"name\":\"" << jsonEscape(l.device) << "\""
              << ",\"transport\":\"" << l.transport << "\""
              << ",\"error\":\"" << jsonEscape(l.error) << "\""
              << ",\"encoder\":\"" << jsonEscape(l.encoder) << "\""
              << ",\"codec\":\"" << l.codec << "\""
              << ",\"source\":\"" << jsonEscape(l.sourceName) << "\""
              << ",\"displayW\":" << l.displayW << ",\"displayH\":" << l.displayH << ",\"hz\":" << l.hz
              << ",\"encodeW\":" << l.encodeW << ",\"encodeH\":" << l.encodeH << ",\"fps\":" << l.fps
              << ",\"kbps\":" << l.kbps << ",\"measuredKbps\":" << l.measuredKbps
              << ",\"rttMs\":" << l.rttMs << ",\"decodeMs\":" << l.decodeMs
              << ",\"encodeMs\":" << l.encodeMs << ",\"measuredFps\":" << l.measuredFps
              << ",\"lossPercent\":" << l.lossPercent << ",\"framesDropped\":" << l.framesDropped
              << ",\"usingVirtualDisplay\":" << (l.usingVirtualDisplay ? "true" : "false")
              << ",\"captureGpu\":\"" << jsonEscape(l.captureGpu) << "\",\"encoderGpu\":\"" << jsonEscape(l.encoderGpu) << "\""
              << ",\"crossAdapter\":" << (l.crossAdapter ? "true" : "false")
              << ",\"encoderFallback\":" << (l.encoderFallback ? "true" : "false")
              << ",\"settings\":{" << settingsJson(d.width, d.height, d.hz, d.maxKbps, d.mirror, d.scalePercent, d.encoder, d.fill) << "}"
              << ",\"options\":" << optionsJson(l, pc) << "}";
        }
    }
    o << "]"
      << ",\"consoleAutostart\":" << (consoleStartsWithWindows() ? "true" : "false")
      << ",\"consoleAutostartAsked\":" << (settings.consoleAutostartAsked ? "true" : "false")
      << ",\"virtualDisplayInstalled\":" << (vddInstalled() ? "true" : "false")
      << ",\"adbFound\":" << (adb_.adbFound() ? "true" : "false")
      << ",\"pairingCode\":\"" << code << "\",\"pairingSeconds\":" << codeSeconds;
    {
        const UpdateStatus u = updateStatus();
        o << ",\"update\":{\"state\":\"" << u.state << "\",\"version\":\"" << u.version << "\",\"notes\":\"" << jsonEscape(u.notes)
          << "\",\"progress\":" << u.progress << "}";
    }
    {
        o << ",\"vddGpu\":\"" << jsonEscape(settings.vddGpu) << "\",\"vddGpuEditable\":" << (vddDriverIsOurs() ? "true" : "false") << ",\"gpus\":[";
        std::vector<std::string> names;
        for (const auto& g : listGpus()) {
            const std::string name = narrow(g.name);
            if (std::find(names.begin(), names.end(), name) != names.end()) continue;
            if (!names.empty()) o << ',';
            names.push_back(name);
            o << '"' << jsonEscape(name) << '"';
        }
        o << "],\"encoders\":[";
        bool firstEncoder = true;
        for (const auto& e : encoderChoices()) {
            if (!firstEncoder) o << ',';
            firstEncoder = false;
            o << "{\"id\":\"" << jsonEscape(e.id) << "\",\"label\":\"" << jsonEscape(e.label) << "\"}";
        }
        o << "]";
    }
    o << ",\"settings\":{" << settingsJson(settings.width, settings.height, settings.hz, settings.maxKbps, settings.mirror, settings.scalePercent, settings.encoder, settings.fill)
      << ",\"allowWireless\":" << (settings.allowWireless ? "true" : "false") << "}"
      << ",\"options\":" << optionsJson(LiveStatus{}, pc);
    o << ",\"usbDevices\":[";
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

std::vector<LiveStatus> Host::liveStatuses() {
    std::vector<LiveStatus> out;
    std::lock_guard<std::mutex> lock(sessionMutex_);
    for (auto& c : connections_)
        if (c->session) out.push_back(c->session->live());
    return out;
}

LiveStatus Host::liveStatus() {
    auto all = liveStatuses();
    if (!all.empty()) return all.front();
    LiveStatus l;
    std::lock_guard<std::mutex> lock(statusMutex_);
    l.error = hostError_;
    return l;
}

std::vector<std::pair<LiveStatus, DeviceSettings>> Host::deviceViews() {
    std::vector<std::pair<LiveStatus, DeviceSettings>> out;
    std::lock_guard<std::mutex> lock(sessionMutex_);
    for (auto& c : connections_)
        if (c->session) out.emplace_back(c->session->live(), c->session->settings());
    return out;
}

void Host::chooseForDevice(const std::string& id, const Settings& s) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    for (auto& c : connections_)
        if (c->session && c->session->id() == id)
            c->session->chooseDisplay(s.width, s.height, s.hz, s.maxKbps, s.mirror, s.scalePercent, s.encoder, s.fill);
}

void Host::setAllowWireless(bool allow) {
    Settings s = store_.settings();
    if (s.allowWireless == allow) return;
    s.allowWireless = allow;
    store_.setSettings(s);
    if (opts_.enableDiscovery) {
        if (allow) discovery_.start(opts_.port);
        else discovery_.stop();
    }
}

bool Host::setVddGpu(const std::string& name) {
    const auto all = listGpus();
    if (!name.empty() && std::none_of(all.begin(), all.end(), [&](const auto& g) { return narrow(g.name) == name; })) return false;
    Settings s = store_.settings();
    if (s.vddGpu == name) return true;
    s.vddGpu = name;
    store_.setSettings(s);
    // The next round of each pipeline applies it, by way of prepareDriver.
    std::lock_guard<std::mutex> lock(sessionMutex_);
    for (auto& c : connections_)
        if (c->session) c->session->displaysChanged();
    return true;
}

void Host::applySettings(const Settings& s) {
    setAllowWireless(s.allowWireless);
    store_.setSettings(s);
    store_.clearCustomDeviceSettings();
    std::lock_guard<std::mutex> lock(sessionMutex_);
    for (auto& c : connections_)
        if (c->session) c->session->followDefaults(s);
}

void Host::disconnectPhone(const std::string& id) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    for (auto& c : connections_)
        if (c->session && (id.empty() || c->session->id() == id)) c->session->requestStop();
}

static constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t kConsoleRunValue[] = L"SideDroidConsole";

bool Host::consoleStartsWithWindows() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kConsoleRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void Host::setConsoleStartsWithWindows(bool on) {
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kConsoleRunValue);
        return;
    }
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    dir.erase(dir.find_last_of(L'\\'));
    const std::wstring command = L"\"" + dir + L"\\console\\SideDroid.exe\"";
    const LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kConsoleRunValue, REG_SZ, command.c_str(),
                                      static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (r != ERROR_SUCCESS) logf("cannot set the console to start with Windows: %ld", r);
}

std::string Host::startPairing() {
    std::lock_guard<std::mutex> lock(pairMutex_);
    pairingCode_ = generatePairingCode();
    pairingExpiresUs_ = monotonicUs() + kPairingLifetimeUs;
    pairingAttempts_ = 0;
    return pairingCode_;
}

std::string Host::handleCommand(const std::string& json) {
    const std::string cmd = jsonGetString(json, "cmd");
    if (cmd == "status") return statusJson();
    if (cmd == "set") {
        // With an id, for that device. Without, new defaults for all of them.
        const std::string id = jsonGetString(json, "id");
        if (!id.empty()) {
            setAllowWireless(jsonGetBool(json, "allowWireless", store_.settings().allowWireless));
            std::lock_guard<std::mutex> lock(sessionMutex_);
            for (auto& c : connections_) {
                if (!c->session || c->session->id() != id) continue;
                const DeviceSettings d = c->session->settings();
                c->session->chooseDisplay(static_cast<uint16_t>(jsonGetNumber(json, "width", d.width)),
                                          static_cast<uint16_t>(jsonGetNumber(json, "height", d.height)),
                                          static_cast<uint16_t>(jsonGetNumber(json, "hz", d.hz)),
                                          static_cast<uint32_t>(jsonGetNumber(json, "maxKbps", d.maxKbps)),
                                          jsonGetBool(json, "mirror", d.mirror),
                                          static_cast<uint16_t>(jsonGetNumber(json, "scale", d.scalePercent)),
                                          json.find("\"encoder\"") != std::string::npos ? jsonGetString(json, "encoder") : d.encoder,
                                          static_cast<uint8_t>(jsonGetNumber(json, "fill", d.fill)));
                return R"({"ok":true})";
            }
            return R"({"ok":false,"error":"no such device"})";
        }
        Settings s = store_.settings();
        const Settings before = s;
        s.width = static_cast<uint16_t>(jsonGetNumber(json, "width", s.width));
        s.height = static_cast<uint16_t>(jsonGetNumber(json, "height", s.height));
        s.hz = static_cast<uint16_t>(jsonGetNumber(json, "hz", s.hz));
        s.maxKbps = static_cast<uint32_t>(jsonGetNumber(json, "maxKbps", s.maxKbps));
        s.allowWireless = jsonGetBool(json, "allowWireless", s.allowWireless);
        s.mirror = jsonGetBool(json, "mirror", s.mirror);
        s.scalePercent = static_cast<uint16_t>(jsonGetNumber(json, "scale", s.scalePercent));
        s.fill = static_cast<uint8_t>(jsonGetNumber(json, "fill", s.fill));
        if (s.fill > kFillTile) s.fill = kFillFit;
        if (json.find("\"encoder\"") != std::string::npos) s.encoder = jsonGetString(json, "encoder");
        // Turning wireless on or off is no reason to forget what each device has chosen.
        if (s.width == before.width && s.height == before.height && s.hz == before.hz && s.maxKbps == before.maxKbps &&
            s.mirror == before.mirror && s.scalePercent == before.scalePercent && s.encoder == before.encoder && s.fill == before.fill) {
            setAllowWireless(s.allowWireless);
        } else {
            applySettings(s);
        }
        return R"({"ok":true})";
    }
    if (cmd == "pair_start") {
        return std::string(R"({"ok":true,"code":")") + startPairing() + "\"}";
    }
    if (cmd == "pair_cancel") {
        std::lock_guard<std::mutex> lock(pairMutex_);
        pairingCode_.clear();
        return R"({"ok":true})";
    }
    if (cmd == "install_driver") {
        // Runs the bundled tool elevated: Windows asks the user, twice if it must
        // (once for elevation, once for the driver's publisher).
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir(self);
        dir.erase(dir.find_last_of(L'\\'));
        const std::wstring tool = dir + L"\\sidedroid-driver-tool.exe";
        const std::wstring args = L"install \"" + dir + L"\\driver\"";
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.lpVerb = L"runas";
        sei.lpFile = tool.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        const bool started = ShellExecuteExW(&sei) != FALSE;
        if (!started) logf("cannot start driver tool: %lu", GetLastError());
        return std::string(R"({"ok":)") + (started ? "true" : "false") + "}";
    }
    if (cmd == "unpair") {
        bool removed = store_.removePaired(jsonGetString(json, "id"));
        return std::string(R"({"ok":)") + (removed ? "true" : "false") + "}";
    }
    if (cmd == "disconnect") {
        disconnectPhone(jsonGetString(json, "id"));
        return R"({"ok":true})";
    }
    if (cmd == "set_gpu") {
        return std::string(R"({"ok":)") + (setVddGpu(jsonGetString(json, "gpu")) ? "true" : "false") + "}";
    }
    if (cmd == "console_autostart") {
        setConsoleStartsWithWindows(jsonGetBool(json, "enabled", false));
        Settings s = store_.settings();
        s.consoleAutostartAsked = true;
        store_.setSettings(s);
        return R"({"ok":true})";
    }
    if (cmd == "update_install") {
        return std::string(R"({"ok":)") + (installUpdate() ? "true" : "false") + "}";
    }
    return R"({"ok":false,"error":"unknown command"})";
}

}  // namespace sd
