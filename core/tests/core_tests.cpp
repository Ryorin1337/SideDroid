// Dependency-free tests for the protocol core. Exits non-zero on failure.
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#include "sidedroid/client.h"
#include "sidedroid/fec.h"
#include "sidedroid/net.h"
#include "sidedroid/ratecontrol.h"
#include "sidedroid/secure.h"
#include "sidedroid/wire.h"

using namespace sd;

static int g_failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

static void testWire() {
    Hello h;
    h.deviceName = "Pixel";
    h.nativeWidth = 2400;
    h.nativeHeight = 1080;
    h.densityDpi = 420;
    h.codecs = kCodecH264 | kCodecHevc;
    h.maxDecodeWidth = 3840;
    h.maxDecodeHeight = 2160;
    h.refreshRates = {60, 90, 120};
    h.wantUdp = true;
    auto b = encode(h);
    CHECK(b[0] == static_cast<uint8_t>(MsgType::Hello));
    auto d = decodeHello(Reader(b.data() + 1, b.size() - 1));
    CHECK(d.has_value());
    CHECK(d->deviceName == "Pixel" && d->refreshRates.size() == 3 && d->refreshRates[2] == 120 && d->wantUdp);

    // Every truncation must fail cleanly rather than read out of bounds.
    for (size_t n = 1; n < b.size(); ++n) CHECK(!decodeHello(Reader(b.data() + 1, n - 1)).has_value());

    std::vector<TouchPoint> pts{{3, TouchAction::Down, 100, 65535, 512}, {4, TouchAction::Up, 0, 1, 0}};
    auto tb = encodeTouch(pts);
    auto td = decodeTouch(Reader(tb.data() + 1, tb.size() - 1));
    CHECK(td && td->size() == 2 && (*td)[0].y == 65535 && (*td)[1].action == TouchAction::Up);
    tb[3] = 9;  // invalid action
    CHECK(!decodeTouch(Reader(tb.data() + 1, tb.size() - 1)).has_value());

    DisplayOptions opt;
    opt.phone = {3168, 1440};
    opt.pc = {2560, 1600};
    opt.recommendedScale = 225;
    opt.currentScale = 150;
    opt.resolutions = {{1920, 1080}, {1280, 720}};
    auto ob = encode(opt);
    auto od = decodeDisplayOptions(Reader(ob.data() + 1, ob.size() - 1));
    CHECK(od && od->phone.width == 3168 && od->currentScale == 150 && od->resolutions.size() == 2 && od->resolutions[1].height == 720);
    for (size_t n = 1; n < ob.size(); ++n) CHECK(!decodeDisplayOptions(Reader(ob.data() + 1, n - 1)).has_value());

    uint8_t payload[] = {1, 2, 3};
    FrameHeader fh{7, 123456789012ull, true};
    auto fb = encodeFrame(fh, payload, 3);
    FrameHeader fo;
    const uint8_t* p;
    size_t n;
    CHECK(decodeFrame(Reader(fb.data() + 1, fb.size() - 1), fo, p, n));
    CHECK(fo.frameId == 7 && fo.captureUs == 123456789012ull && fo.keyframe && n == 3 && p[2] == 3);
}

static void testReedSolomon() {
    std::mt19937 rng(1);
    for (int k : {1, 2, 8, 32}) {
        for (int m : {1, 3, 8}) {
            const size_t size = 64;
            std::vector<Bytes> shards(k + m, Bytes(size));
            std::vector<const uint8_t*> d;
            std::vector<uint8_t*> par;
            for (int i = 0; i < k; ++i) {
                for (auto& x : shards[i]) x = static_cast<uint8_t>(rng());
                d.push_back(shards[i].data());
            }
            for (int i = 0; i < m; ++i) par.push_back(shards[k + i].data());
            rs::encode(k, m, d, par, size);
            auto original = shards;

            // Lose exactly m shards, chosen at random.
            for (int trial = 0; trial < 20; ++trial) {
                auto work = original;
                std::vector<bool> present(k + m, true);
                for (int lost = 0; lost < m;) {
                    int i = rng() % (k + m);
                    if (present[i]) {
                        present[i] = false;
                        std::memset(work[i].data(), 0xEE, size);
                        ++lost;
                    }
                }
                std::vector<uint8_t*> ptrs;
                for (auto& s : work) ptrs.push_back(s.data());
                CHECK(rs::reconstruct(k, m, ptrs, present, size));
                for (int i = 0; i < k; ++i) CHECK(work[i] == original[i]);
            }
            // One too many must be refused when all lost shards leave < k.
            std::vector<bool> present(k + m, true);
            for (int i = 0; i < m + 1 && i < k + m; ++i) present[i] = false;
            if (m + 1 <= k) {
                std::vector<uint8_t*> ptrs;
                auto work = original;
                for (auto& s : work) ptrs.push_back(s.data());
                CHECK(!rs::reconstruct(k, m, ptrs, present, size));
            }
        }
    }
}

struct AssemblerHarness {
    std::vector<std::pair<uint32_t, Bytes>> frames;
    std::vector<uint32_t> drops;
    std::vector<Nack> nacks;
    FrameAssembler a;
    AssemblerHarness()
        : a(FrameAssembler::Callbacks{
              [this](uint32_t id, Bytes&& d) { frames.emplace_back(id, std::move(d)); },
              [this](const Nack& n) { nacks.push_back(n); },
              [this](uint32_t id) { drops.push_back(id); }}) {}
};

static Bytes randomBytes(size_t n, uint32_t seed) {
    std::mt19937 r(seed);
    Bytes b(n);
    for (auto& x : b) x = static_cast<uint8_t>(r());
    return b;
}

static void testAssembler() {
    // No loss, sizes around block and shard boundaries.
    for (size_t size : std::vector<size_t>{1, kShardPayload - 1, kShardPayload, kShardPayload + 1,
                                           kShardPayload * kBlockDataShards,
                                           kShardPayload * kBlockDataShards + 1, 250000}) {
        AssemblerHarness h;
        auto data = randomBytes(size, static_cast<uint32_t>(size));
        for (auto& p : packetize(10, data.data(), data.size(), 10)) h.a.onPacket(p.data(), p.size(), 1000);
        CHECK(h.frames.size() == 1 && h.frames[0].first == 10 && h.frames[0].second == data);
    }

    // Loss within FEC capacity needs no retransmission.
    {
        AssemblerHarness h;
        auto data = randomBytes(30000, 3);
        auto pkts = packetize(1, data.data(), data.size(), 25);
        int i = 0;
        for (auto& p : pkts) {
            if (i++ % 7 != 3) h.a.onPacket(p.data(), p.size(), 1000);
        }
        CHECK(h.frames.size() == 1 && h.frames[0].second == data);
    }

    // Loss beyond FEC is NACKed; retransmits complete the frame.
    {
        AssemblerHarness h;
        RetransmitBuffer rb;
        auto data = randomBytes(30000, 4);
        auto pkts = packetize(5, data.data(), data.size(), 10);
        rb.store(5, pkts);
        for (size_t i = 5; i < pkts.size(); ++i) h.a.onPacket(pkts[i].data(), pkts[i].size(), 1000);  // 3 parity shards cannot cover 5 losses
        CHECK(h.frames.empty());
        h.a.poll(1000 + 10000);
        CHECK(!h.nacks.empty());
        if (h.nacks.empty()) return;
        for (auto& it : h.nacks[0].items) {
            auto p = rb.find(5, it.block, it.shard);
            CHECK(!p.empty());
            h.a.onPacket(p.data(), p.size(), 12000);
        }
        CHECK(h.frames.size() == 1 && h.frames[0].second == data);
    }

    // Unrecoverable frame is dropped, later frames still flow in order.
    {
        AssemblerHarness h;
        auto d1 = randomBytes(5000, 5), d2 = randomBytes(5000, 6);
        auto p1 = packetize(1, d1.data(), d1.size(), 0);
        auto p2 = packetize(2, d2.data(), d2.size(), 0);
        h.a.onPacket(p1[0].data(), p1[0].size(), 1000);  // rest of frame 1 is lost
        for (auto& p : p2) h.a.onPacket(p.data(), p.size(), 2000);
        CHECK(h.frames.empty());  // frame 2 waits for frame 1
        h.a.poll(1000 + 130000);
        CHECK(h.drops.size() == 1 && h.drops[0] == 1);
        CHECK(h.frames.size() == 1 && h.frames[0].first == 2 && h.frames[0].second == d2);
    }

    // Malformed packets must not crash or produce frames.
    {
        AssemblerHarness h;
        std::mt19937 r(9);
        for (int i = 0; i < 2000; ++i) {
            Bytes junk((r() % 2) ? kPacketHeader + kShardPayload : r() % 64);
            for (auto& x : junk) x = static_cast<uint8_t>(r());
            h.a.onPacket(junk.data(), junk.size(), 1000 + i);
            h.a.poll(1000 + i);
        }
        CHECK(h.drops.size() <= 2000 * 1025);  // gaps stay bounded, whatever the junk claims
    }
}

static void testRatecontrol() {
    RateLimits l;
    l.minKbps = 2000;
    l.maxKbps = 20000;
    l.startKbps = 10000;
    l.maxFps = 60;
    l.minFps = 30;
    l.udp = true;
    RateController rc(l);

    Stats good{};
    good.windowMs = 100;
    good.packetsReceived = 1000;
    good.bytes = 1250000;  // 100 Mbps, so the sender is really using the rate it asks for
    uint32_t start = rc.target().bitrateKbps;
    for (int i = 0; i < 50; ++i) rc.update(good, 0);
    CHECK(rc.target().bitrateKbps > start);
    CHECK(rc.target().fps == 60 && rc.target().scalePercent == 100);

    // Sustained heavy loss: bitrate falls first, then fps, then scale.
    Stats bad = good;
    bad.packetsLost = 200;
    int firstFpsDropAtKbps = -1;
    for (int i = 0; i < 400; ++i) {
        rc.update(bad, 0);
        if (rc.target().fps < 60 && firstFpsDropAtKbps < 0) firstFpsDropAtKbps = static_cast<int>(rc.target().bitrateKbps);
    }
    CHECK(firstFpsDropAtKbps == 2000);
    CHECK(rc.target().bitrateKbps == 2000);
    CHECK(rc.target().fps == 30);
    CHECK(rc.target().scalePercent < 100);
    CHECK(rc.target().parityPercent >= 20);

    // Clean network again: everything recovers in the opposite order.
    for (int i = 0; i < 3000; ++i) rc.update(good, 0);
    CHECK(rc.target().scalePercent == 100);
    CHECK(rc.target().fps == 60);
    CHECK(rc.target().bitrateKbps == 20000);
    CHECK(rc.target().parityPercent == 5);

    // Random loss below the congestion threshold is left to FEC.
    {
        RateController lossy(l);
        Stats s = good;
        s.packetsLost = 50;  // about 5%
        uint32_t b0 = lossy.target().bitrateKbps;
        for (int i = 0; i < 100; ++i) lossy.update(s, 0);
        CHECK(lossy.target().bitrateKbps >= b0);
        CHECK(lossy.target().scalePercent == 100 && lossy.target().fps == 60);
        CHECK(lossy.target().parityPercent >= 20);
    }
    // Jitter around a constant queue is not congestion; steady growth is.
    {
        RateController j(l);
        Stats s = good;
        uint32_t b0 = j.target().bitrateKbps;
        for (int i = 0; i < 100; ++i) {
            s.delayGradientUs = (i % 2) ? 6000 : -6000;
            j.update(s, 0);
        }
        CHECK(j.target().bitrateKbps >= b0);
        s.delayGradientUs = 5000;
        uint32_t b1 = j.target().bitrateKbps;
        for (int i = 0; i < 20; ++i) j.update(s, 0);
        CHECK(j.target().bitrateKbps < b1);
    }
    // An idle desktop neither raises nor lowers a rate at its start value.
    {
        RateController idle(l);
        Stats s{};
        s.windowMs = 100;
        uint32_t b0 = idle.target().bitrateKbps;
        for (int i = 0; i < 100; ++i) idle.update(s, 0);
        CHECK(idle.target().bitrateKbps == b0);
    }

    // Sender backlog alone counts as congestion on TCP, where loss is invisible.
    RateLimits t = l;
    t.udp = false;
    RateController rt(t);
    uint32_t before = rt.target().bitrateKbps;
    for (int i = 0; i < 20; ++i) rt.update(good, 100);
    CHECK(rt.target().bitrateKbps < before);
    CHECK(rt.target().parityPercent == 0);
}

static void testSecure() {
    CHECK(normalizePairingCode("k7qm-2xpd") == "K7QM2XPD");
    CHECK(normalizePairingCode("O1il") == "0111");
    auto code = generatePairingCode();
    CHECK(code.size() == 9 && code[4] == '-');
    CHECK(normalizePairingCode(code).size() == 8);

    Key32 key{};
    key[0] = 42;
    uint8_t plain[5] = {1, 2, 3, 4, 5};
    uint8_t sealed[5 + kUdpOverhead], opened[5];
    uint64_t seq = 0;
    udpSeal(key, 77, plain, 5, sealed);
    CHECK(udpOpen(key, sealed, sizeof(sealed), opened, &seq) && seq == 77 && std::memcmp(plain, opened, 5) == 0);
    sealed[kUdpOverhead] ^= 1;
    CHECK(!udpOpen(key, sealed, sizeof(sealed), opened, &seq));
    sealed[kUdpOverhead] ^= 1;
    sealed[0] ^= 1;  // sequence number is authenticated
    CHECK(!udpOpen(key, sealed, sizeof(sealed), opened, &seq));

    ReplayWindow w;
    CHECK(w.accept(100));
    CHECK(!w.accept(100));
    CHECK(w.accept(99));
    CHECK(!w.accept(99));
    CHECK(w.accept(300));
    CHECK(!w.accept(100));
}

// Runs one handshake over a real loopback socket.
struct HsOutcome { bool client = false, host = false; HandshakeResult c, h; };
static HsOutcome runHandshake(AuthMode mode, const std::string& clientCode, const std::string& hostCode,
                              const Key32& clientPsk, std::optional<Key32> hostPsk, bool loopback) {
    HsOutcome out;
    Socket l = Socket::tcpListen(0 + 27999, true);
    CHECK(l.valid());
    std::thread host([&] {
        Socket s = l.accept(5000, nullptr, nullptr);
        HostAuth a;
        a.peerIsLoopback = loopback;
        a.currentPairingCode = [&]() -> std::optional<std::string> {
            if (hostCode.empty()) return std::nullopt;
            return hostCode;
        };
        a.lookupPaired = [&](const ClientId&) { return hostPsk; };
        auto r = handshakeHost(s, a);
        if (r) { out.host = true; out.h = *r; }
        s.shutdownBoth();
    });
    Socket c = Socket::tcpConnect("127.0.0.1", 27999, 2000);
    ClientId id{};
    id[0] = 7;
    std::function<Key32(const uint8_t*, const uint8_t*)> derive;
    if (mode == AuthMode::Pairing)
        derive = [&](const uint8_t* a, const uint8_t* b) { return pskFromPairingCode(clientCode, a, b); };
    auto r = handshakeClient(c, mode, id, clientPsk, derive);
    if (r) { out.client = true; out.c = *r; }
    c.shutdownBoth();
    host.join();
    return out;
}

static void testHandshake() {
    Key32 zero{}, secret{};
    secret[3] = 9;

    auto usb = runHandshake(AuthMode::Usb, "", "", zero, std::nullopt, true);
    CHECK(usb.client && usb.host && usb.c.keys.tcpC2H == usb.h.keys.tcpC2H && usb.c.keys.udpH2C == usb.h.keys.udpH2C);
    CHECK(usb.c.keys.tcpC2H != usb.c.keys.tcpH2C);

    auto usbRemote = runHandshake(AuthMode::Usb, "", "", zero, std::nullopt, false);
    CHECK(!usbRemote.client && !usbRemote.host);

    auto pair = runHandshake(AuthMode::Pairing, "abcd-2345", "ABCD2345", zero, std::nullopt, false);
    CHECK(pair.client && pair.host && pair.c.keys.pairSecret == pair.h.keys.pairSecret);

    auto wrong = runHandshake(AuthMode::Pairing, "ABCD-2346", "ABCD-2345", zero, std::nullopt, false);
    CHECK(!wrong.client && !wrong.host);

    auto noCode = runHandshake(AuthMode::Pairing, "ABCD-2345", "", zero, std::nullopt, false);
    CHECK(!noCode.client && !noCode.host);

    auto paired = runHandshake(AuthMode::Paired, "", "", secret, secret, false);
    CHECK(paired.client && paired.host);

    auto badPaired = runHandshake(AuthMode::Paired, "", "", zero, secret, false);
    CHECK(!badPaired.client && !badPaired.host);

    auto unknownPaired = runHandshake(AuthMode::Paired, "", "", secret, std::nullopt, false);
    CHECK(!unknownPaired.client && !unknownPaired.host);
}

static void testChannel() {
    Socket l = Socket::tcpListen(27998, true);
    Key32 a{}, b{};
    a[0] = 1;
    b[0] = 2;
    Bytes big = randomBytes(1'000'000, 11);
    Bytes received;
    bool ok = false;
    std::thread t([&] {
        Socket s = l.accept(5000, nullptr, nullptr);
        SecureChannel ch(std::move(s), b, a);
        ok = ch.recv(received) && ch.send(received);
    });
    SecureChannel ch(Socket::tcpConnect("127.0.0.1", 27998, 2000), a, b);
    CHECK(ch.send(big));
    Bytes echo;
    CHECK(ch.recv(echo));
    t.join();
    CHECK(ok && echo == big);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    netInit();
    const struct { const char* name; void (*fn)(); } tests[] = {
        {"wire", testWire}, {"reed-solomon", testReedSolomon}, {"assembler", testAssembler},
        {"ratecontrol", testRatecontrol}, {"secure", testSecure}, {"handshake", testHandshake},
        {"channel", testChannel}};
    for (const auto& t : tests) {
        std::printf("[ RUN ] %s\n", t.name);
        std::fflush(stdout);
        uint64_t begin = monotonicUs();
        t.fn();
        std::printf("[ END ] %s (%llu ms)\n", t.name, static_cast<unsigned long long>((monotonicUs() - begin) / 1000));
        std::fflush(stdout);
    }
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("core_tests: all passed\n");
    return 0;
}
