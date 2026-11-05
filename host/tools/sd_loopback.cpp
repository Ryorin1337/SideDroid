// Stream client for testing without a phone. It uses the same core client as the
// Android app and writes what it receives to an Annex B file for ffprobe to check.
//
//   sd-loopback --port 27183 --seconds 10 --out out.h265 [--udp] [--drop 5] [--limit-kbps 4000]
//               [--client-id 7] [--name Tablet]   to act as one particular device
//               [--shared-limit name 30000]       a link of this many kbps, shared by all clients giving this name
//               [--rates 60,120]                  refresh rates the "phone" reports
//
// Host and client share one clock on the same machine, so the reported latency is
// real: capture on the host until the frame has been received here. Decoding and
// display are not included.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "sidedroid/client.h"

using namespace sd;

namespace {

// A token bucket in shared memory, so that separate client processes contend for one link
// the way phones on one access point do. Tail drop when it is empty, like a full queue.
struct SharedBucket {
    volatile LONG lock;
    double tokens;
    LONGLONG lastCounter;
};

SharedBucket* openBucket(const std::string& name) {
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedBucket), ("Local\\sd-link-" + name).c_str());
    return map ? static_cast<SharedBucket*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBucket))) : nullptr;
}

bool takeFromBucket(SharedBucket* b, uint32_t kbps, int bytes) {
    static const double perSecond = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    constexpr double kDepthBytes = 128000;  // about what an access point queues
    while (InterlockedCompareExchange(&b->lock, 1, 0) != 0) YieldProcessor();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (b->lastCounter != 0) b->tokens = (std::min)(b->tokens + (now.QuadPart - b->lastCounter) / perSecond * kbps * 125.0, kDepthBytes);
    else b->tokens = kDepthBytes;
    b->lastCounter = now.QuadPart;
    const bool ok = b->tokens >= bytes;
    if (ok) b->tokens -= bytes;
    InterlockedExchange(&b->lock, 0);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    int seconds = 10;
    uint16_t port = kDefaultPort;
    bool udp = false;
    int drop = 0;
    uint32_t limit = 0;
    std::string outPath;
    uint16_t w = 1920, h = 1080;
    uint8_t codecs = kCodecHevc | kCodecH264;
    bool requestKey = false;
    int mirrorAt = -1, extendAt = -1, scaleAt = -1;
    uint16_t scaleTo = 0, density = 160;
    bool commands = false;
    int clientId = 0;
    std::string name = "sd-loopback";
    std::string sharedName;
    uint32_t sharedKbps = 0;
    std::vector<uint16_t> rates{60};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--seconds") seconds = atoi(next());
        else if (a == "--port") port = static_cast<uint16_t>(atoi(next()));
        else if (a == "--udp") udp = true;
        else if (a == "--drop") drop = atoi(next());
        else if (a == "--limit-kbps") limit = static_cast<uint32_t>(atoi(next()));
        else if (a == "--out") outPath = next();
        else if (a == "--size") { w = static_cast<uint16_t>(atoi(next())); h = static_cast<uint16_t>(atoi(next())); }
        else if (a == "--h264") codecs = kCodecH264;
        else if (a == "--request-key") requestKey = true;
        else if (a == "--density") density = static_cast<uint16_t>(atoi(next()));
        else if (a == "--scale-at") { scaleAt = atoi(next()); scaleTo = static_cast<uint16_t>(atoi(next())); }
        else if (a == "--mirror-at") mirrorAt = atoi(next());
        else if (a == "--extend-at") extendAt = atoi(next());
        else if (a == "--commands") commands = true;
        else if (a == "--client-id") clientId = atoi(next());
        else if (a == "--name") name = next();
        else if (a == "--shared-limit") { sharedName = next(); sharedKbps = static_cast<uint32_t>(atoi(next())); }
        else if (a == "--rates") {
            rates.clear();
            std::string list = next();
            for (size_t p = 0; p < list.size();) {
                rates.push_back(static_cast<uint16_t>(atoi(list.c_str() + p)));
                const size_t comma = list.find(',', p);
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
        }
    }

    FILE* out = outPath.empty() ? nullptr : fopen(outPath.c_str(), "wb");

    ClientOptions opts;
    opts.port = port;
    opts.mode = AuthMode::Usb;
    opts.clientId[0] = static_cast<uint8_t>(clientId);
    opts.hello.deviceName = name;
    opts.hello.nativeWidth = w;
    opts.hello.nativeHeight = h;
    opts.hello.densityDpi = density;
    opts.hello.codecs = codecs;
    opts.hello.maxDecodeWidth = 3840;
    opts.hello.maxDecodeHeight = 2160;
    opts.hello.refreshRates = rates;
    opts.hello.wantUdp = udp;
    opts.debugDropPercent = drop;
    opts.debugLimitKbps = limit;
    if (!sharedName.empty()) {
        SharedBucket* bucket = openBucket(sharedName);
        if (bucket) opts.debugSharedLink = [bucket, sharedKbps](int bytes) { return takeFromBucket(bucket, sharedKbps, bytes); };
    }

    std::mutex m;
    std::vector<int64_t> latencies;
    std::atomic<uint64_t> frames{0}, keys{0}, bytes{0}, configs{0};
    Config lastCfg;
    std::vector<uint64_t> kbpsPerSecond;
    std::atomic<uint64_t> secondBytes{0};

    Client::Callbacks cb;
    cb.onConfig = [&](const Config& c) {
        std::lock_guard<std::mutex> lock(m);
        lastCfg = c;
        configs.fetch_add(1);
        printf("config: display %dx%d encode %dx%d @%d codec=%s %u kbps mirror=%d fill=%d\n", c.displayWidth, c.displayHeight,
               c.encodeWidth, c.encodeHeight, c.fps, c.codec == kCodecHevc ? "HEVC" : "H.264", c.bitrateKbps, c.mirror ? 1 : 0, c.fill);
        fflush(stdout);
    };
    cb.onFrame = [&](const FrameHeader& fh, const uint8_t* d, size_t n) {
        frames.fetch_add(1);
        if (fh.keyframe) keys.fetch_add(1);
        bytes.fetch_add(n);
        secondBytes.fetch_add(n);
        std::lock_guard<std::mutex> lock(m);
        latencies.push_back(static_cast<int64_t>(monotonicUs() - fh.captureUs));
        if (out) fwrite(d, 1, n, out);
    };
    cb.onDisplayOptions = [&](const DisplayOptions& o) {
        printf("options: phone %dx%d pc %dx%d scale recommended=%d current=%d sizes=%zu\n", o.phone.width, o.phone.height,
               o.pc.width, o.pc.height, o.recommendedScale, o.currentScale, o.resolutions.size());
        fflush(stdout);
    };
    std::atomic<bool> gone{false};
    cb.onDisconnected = [&](const std::string& why) {
        printf("disconnected: %s\n", why.c_str());
        fflush(stdout);
        gone = true;
    };

    Client client(opts, cb);
    std::string error;
    if (!client.start(&error)) {
        printf("connect failed: %s\n", error.c_str());
        return 2;
    }

    // --commands: lets a test script act at the moment it chooses, one line each:
    //   tap <x> <y>     0..65535 across the display
    //   size <w> <h>    as the resolution setting does
    //   quit
    std::atomic<bool> quit{false};
    if (commands) {
        std::thread([&] {
            char line[128];
            while (fgets(line, sizeof(line), stdin)) {
                int a = 0, b = 0;
                if (sscanf(line, "tap %d %d", &a, &b) == 2) {
                    TouchPoint p;
                    p.id = 1;
                    p.x = static_cast<uint16_t>(a);
                    p.y = static_cast<uint16_t>(b);
                    p.pressure = 512;
                    p.action = TouchAction::Down;
                    client.sendTouch({p});
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    p.action = TouchAction::Up;
                    client.sendTouch({p});
                } else if (sscanf(line, "size %d %d", &a, &b) == 2) {
                    client.sendSetDisplay(SetDisplay{static_cast<uint16_t>(a), static_cast<uint16_t>(b), 0, kKeepBitrate, kMirrorKeep});
                } else if (sscanf(line, "fill %d", &a) == 1) {
                    SetDisplay sd{0, 0, 0, kKeepBitrate, kMirrorKeep};
                    sd.fill = static_cast<uint8_t>(a);
                    client.sendSetDisplay(sd);
                } else if (strncmp(line, "quit", 4) == 0) {
                    quit = true;
                    return;
                }
            }
        }).detach();
    }

    for (int s = 0; s < seconds && !gone && !quit; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t b = secondBytes.exchange(0);
        kbpsPerSecond.push_back(b * 8 / 1000);
        if (requestKey && s == seconds / 2) client.requestKeyframe();
        if (s == scaleAt) {
            SetDisplay sd{0, 0, 0, kKeepBitrate, kMirrorKeep};
            sd.scale = scaleTo;
            client.sendSetDisplay(sd);
        }
        if (s == mirrorAt) client.sendSetDisplay(SetDisplay{0, 0, 0, kKeepBitrate, kMirrorOn});
        if (s == extendAt) client.sendSetDisplay(SetDisplay{0, 0, 0, kKeepBitrate, kMirrorOff});
        printf("t=%2ds %6llu kbps frames=%llu rtt=%uus\n", s + 1, static_cast<unsigned long long>(b * 8 / 1000),
               static_cast<unsigned long long>(frames.load()), client.lastRttUs());
        fflush(stdout);
    }
    client.stop();
    if (out) fclose(out);

    std::lock_guard<std::mutex> lock(m);
    std::sort(latencies.begin(), latencies.end());
    auto pct = [&](double p) -> double {
        if (latencies.empty()) return 0;
        return latencies[std::min(latencies.size() - 1, static_cast<size_t>(latencies.size() * p))] / 1000.0;
    };
    printf("RESULT frames=%llu dropped=%llu keyframes=%llu bytes=%llu configs=%llu latency_ms p50=%.1f p95=%.1f p99=%.1f\n",
           static_cast<unsigned long long>(frames.load()), static_cast<unsigned long long>(client.totalFramesDropped()),
           static_cast<unsigned long long>(keys.load()),
           static_cast<unsigned long long>(bytes.load()), static_cast<unsigned long long>(configs.load()), pct(.5),
           pct(.95), pct(.99));
    return frames.load() > 0 ? 0 : 1;
}
