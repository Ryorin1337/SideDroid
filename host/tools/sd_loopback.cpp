// Stream client for testing without a phone. It uses the same core client as the
// Android app and writes what it receives to an Annex B file for ffprobe to check.
//
//   sd-loopback --port 27183 --seconds 10 --out out.h265 [--udp] [--drop 5] [--limit-kbps 4000]
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

#include "sidedroid/client.h"

using namespace sd;

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
    }

    FILE* out = outPath.empty() ? nullptr : fopen(outPath.c_str(), "wb");

    ClientOptions opts;
    opts.port = port;
    opts.mode = AuthMode::Usb;
    opts.hello.deviceName = "sd-loopback";
    opts.hello.nativeWidth = w;
    opts.hello.nativeHeight = h;
    opts.hello.densityDpi = density;
    opts.hello.codecs = codecs;
    opts.hello.maxDecodeWidth = 3840;
    opts.hello.maxDecodeHeight = 2160;
    opts.hello.refreshRates = {60};
    opts.hello.wantUdp = udp;
    opts.debugDropPercent = drop;
    opts.debugLimitKbps = limit;

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
        printf("config: display %dx%d encode %dx%d @%d codec=%s %u kbps mirror=%d\n", c.displayWidth, c.displayHeight,
               c.encodeWidth, c.encodeHeight, c.fps, c.codec == kCodecHevc ? "HEVC" : "H.264", c.bitrateKbps, c.mirror ? 1 : 0);
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
