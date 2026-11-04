#include "sidedroid/wire.h"

namespace sd {

static Writer start(Bytes& b, MsgType t) {
    Writer w(b);
    w.u8(static_cast<uint8_t>(t));
    return w;
}

Bytes encode(const Hello& m) {
    Bytes b;
    auto w = start(b, MsgType::Hello);
    w.u16(m.protocol);
    w.str(m.deviceName);
    w.u16(m.nativeWidth);
    w.u16(m.nativeHeight);
    w.u16(m.densityDpi);
    w.u8(m.codecs);
    w.u16(m.maxDecodeWidth);
    w.u16(m.maxDecodeHeight);
    w.u8(static_cast<uint8_t>(m.refreshRates.size()));
    for (auto r : m.refreshRates) w.u16(r);
    w.u8(m.wantUdp ? 1 : 0);
    return b;
}

std::optional<Hello> decodeHello(Reader r) {
    Hello m;
    m.protocol = r.u16();
    m.deviceName = r.str();
    m.nativeWidth = r.u16();
    m.nativeHeight = r.u16();
    m.densityDpi = r.u16();
    m.codecs = r.u8();
    m.maxDecodeWidth = r.u16();
    m.maxDecodeHeight = r.u16();
    int n = r.u8();
    for (int i = 0; i < n && r.ok(); ++i) m.refreshRates.push_back(r.u16());
    m.wantUdp = r.u8() != 0;
    if (!r.ok()) return std::nullopt;
    return m;
}

Bytes encode(const Config& m) {
    Bytes b;
    auto w = start(b, MsgType::Config);
    w.u16(m.displayWidth);
    w.u16(m.displayHeight);
    w.u16(m.encodeWidth);
    w.u16(m.encodeHeight);
    w.u16(m.fps);
    w.u8(m.codec);
    w.u32(m.bitrateKbps);
    return b;
}

std::optional<Config> decodeConfig(Reader r) {
    Config m;
    m.displayWidth = r.u16();
    m.displayHeight = r.u16();
    m.encodeWidth = r.u16();
    m.encodeHeight = r.u16();
    m.fps = r.u16();
    m.codec = r.u8();
    m.bitrateKbps = r.u32();
    if (!r.ok()) return std::nullopt;
    return m;
}

Bytes encodeFrame(const FrameHeader& h, const uint8_t* data, size_t size) {
    Bytes b;
    b.reserve(size + 16);
    auto w = start(b, MsgType::Frame);
    w.u32(h.frameId);
    w.u64(h.captureUs);
    w.u8(h.keyframe ? 1 : 0);
    w.raw(data, size);
    return b;
}

bool decodeFrame(Reader r, FrameHeader& h, const uint8_t*& payload, size_t& size) {
    h.frameId = r.u32();
    h.captureUs = r.u64();
    h.keyframe = r.u8() != 0;
    if (!r.ok()) return false;
    size = r.remaining();
    payload = r.take(size);
    return r.ok();
}

Bytes encode(const Stats& m) {
    Bytes b;
    auto w = start(b, MsgType::Stats);
    w.u32(m.windowMs);
    w.u32(m.bytes);
    w.u32(m.frames);
    w.u32(m.packetsReceived);
    w.u32(m.packetsLost);
    w.u32(m.framesDropped);
    w.i32(m.delayGradientUs);
    w.u32(m.decodeUs);
    w.u32(m.rttUs);
    return b;
}

std::optional<Stats> decodeStats(Reader r) {
    Stats m;
    m.windowMs = r.u32();
    m.bytes = r.u32();
    m.frames = r.u32();
    m.packetsReceived = r.u32();
    m.packetsLost = r.u32();
    m.framesDropped = r.u32();
    m.delayGradientUs = r.i32();
    m.decodeUs = r.u32();
    m.rttUs = r.u32();
    if (!r.ok()) return std::nullopt;
    return m;
}

Bytes encodeTouch(const std::vector<TouchPoint>& pts) {
    Bytes b;
    auto w = start(b, MsgType::Touch);
    w.u8(static_cast<uint8_t>(pts.size()));
    for (const auto& p : pts) {
        w.u8(p.id);
        w.u8(static_cast<uint8_t>(p.action));
        w.u16(p.x);
        w.u16(p.y);
        w.u16(p.pressure);
    }
    return b;
}

std::optional<std::vector<TouchPoint>> decodeTouch(Reader r) {
    std::vector<TouchPoint> pts;
    int n = r.u8();
    for (int i = 0; i < n; ++i) {
        TouchPoint p;
        p.id = r.u8();
        uint8_t a = r.u8();
        p.x = r.u16();
        p.y = r.u16();
        p.pressure = r.u16();
        if (!r.ok() || a > 3) return std::nullopt;
        p.action = static_cast<TouchAction>(a);
        pts.push_back(p);
    }
    if (!r.ok()) return std::nullopt;
    return pts;
}

Bytes encode(const SetDisplay& m) {
    Bytes b;
    auto w = start(b, MsgType::SetDisplay);
    w.u16(m.width);
    w.u16(m.height);
    w.u16(m.hz);
    w.u32(m.maxBitrateKbps);
    return b;
}

std::optional<SetDisplay> decodeSetDisplay(Reader r) {
    SetDisplay m;
    m.width = r.u16();
    m.height = r.u16();
    m.hz = r.u16();
    m.maxBitrateKbps = r.u32();
    if (!r.ok()) return std::nullopt;
    return m;
}

Bytes encode(const Viewport& m) {
    Bytes b;
    auto w = start(b, MsgType::Viewport);
    w.u16(m.width);
    w.u16(m.height);
    return b;
}

std::optional<Viewport> decodeViewport(Reader r) {
    Viewport m;
    m.width = r.u16();
    m.height = r.u16();
    if (!r.ok()) return std::nullopt;
    return m;
}

Bytes encode(const Nack& m) {
    Bytes b;
    auto w = start(b, MsgType::Nack);
    w.u32(m.frameId);
    w.u16(static_cast<uint16_t>(m.items.size()));
    for (const auto& i : m.items) {
        w.u16(i.block);
        w.u8(i.shard);
    }
    return b;
}

std::optional<Nack> decodeNack(Reader r) {
    Nack m;
    m.frameId = r.u32();
    int n = r.u16();
    for (int i = 0; i < n; ++i) {
        Nack::Item it;
        it.block = r.u16();
        it.shard = r.u8();
        if (!r.ok()) return std::nullopt;
        m.items.push_back(it);
    }
    if (!r.ok()) return std::nullopt;
    return m;
}

Bytes encode(const UdpSetup& m) {
    Bytes b;
    auto w = start(b, MsgType::UdpSetup);
    w.u16(m.port);
    return b;
}

std::optional<UdpSetup> decodeUdpSetup(Reader r) {
    UdpSetup m;
    m.port = r.u16();
    if (!r.ok()) return std::nullopt;
    return m;
}

Bytes encodePing(MsgType t, uint64_t timeUs) {
    Bytes b;
    auto w = start(b, t);
    w.u64(timeUs);
    return b;
}

Bytes encodeSimple(MsgType t) {
    Bytes b;
    start(b, t);
    return b;
}

}  // namespace sd
