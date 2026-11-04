// Little-endian byte serialization and the SideDroid message set.
// See docs/protocol.md for the wire format.
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace sd {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 27183;

class Writer {
public:
    explicit Writer(Bytes& out) : out_(out) {}
    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { put(v, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void u64(uint64_t v) { put(v, 8); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void raw(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        out_.insert(out_.end(), b, b + n);
    }
    void str(const std::string& s) {
        u16(static_cast<uint16_t>(s.size() > 0xFFFF ? 0xFFFF : s.size()));
        raw(s.data(), s.size() > 0xFFFF ? 0xFFFF : s.size());
    }

private:
    void put(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    Bytes& out_;
};

// Bounds-checked reader. After any failed read ok() is false and values are 0.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    explicit Reader(const Bytes& b) : p_(b.data()), n_(b.size()) {}
    uint8_t u8() { return static_cast<uint8_t>(get(1)); }
    uint16_t u16() { return static_cast<uint16_t>(get(2)); }
    uint32_t u32() { return static_cast<uint32_t>(get(4)); }
    uint64_t u64() { return get(8); }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    std::string str() {
        size_t len = u16();
        if (!ok_ || len > n_ - i_) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + i_), len);
        i_ += len;
        return s;
    }
    const uint8_t* take(size_t len) {
        if (!ok_ || len > n_ - i_) { ok_ = false; return nullptr; }
        auto r = p_ + i_;
        i_ += len;
        return r;
    }
    size_t remaining() const { return n_ - i_; }
    bool ok() const { return ok_; }

private:
    uint64_t get(int n) {
        if (!ok_ || static_cast<size_t>(n) > n_ - i_) { ok_ = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(p_[i_ + i]) << (8 * i);
        i_ += n;
        return v;
    }
    const uint8_t* p_;
    size_t n_;
    size_t i_ = 0;
    bool ok_ = true;
};

enum class MsgType : uint8_t {
    Hello = 1,
    Config = 2,
    Frame = 3,
    Stats = 4,
    Touch = 5,
    KeyframeRequest = 6,
    SetDisplay = 7,
    Ping = 8,
    Pong = 9,
    Nack = 10,
    UdpSetup = 11,
    Bye = 12,
    Viewport = 13,
    DisplayOptions = 14,
};

enum Codec : uint8_t { kCodecH264 = 1, kCodecHevc = 2 };

struct DisplayMode {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hz = 0;
    bool operator==(const DisplayMode& o) const {
        return width == o.width && height == o.height && hz == o.hz;
    }
};

// Client -> host: what this device is and what it can show.
struct Hello {
    uint16_t protocol = kProtocolVersion;
    std::string deviceName;
    uint16_t nativeWidth = 0;    // current orientation
    uint16_t nativeHeight = 0;
    uint16_t densityDpi = 0;
    uint8_t codecs = 0;          // bitmask of Codec
    uint16_t maxDecodeWidth = 0;
    uint16_t maxDecodeHeight = 0;
    std::vector<uint16_t> refreshRates;
    bool wantUdp = false;
};

// Host -> client: what is being streamed now. Sent before the first frame and
// whenever anything here changes.
struct Config {
    uint16_t displayWidth = 0;
    uint16_t displayHeight = 0;
    uint16_t encodeWidth = 0;
    uint16_t encodeHeight = 0;
    uint16_t fps = 0;
    uint8_t codec = 0;
    uint32_t bitrateKbps = 0;
    bool mirror = false;      // showing the main display rather than an extended one
    // What the user chose, so that the phone's controls show the real state.
    uint16_t manualWidth = 0;
    uint16_t manualHeight = 0;
    uint16_t manualHz = 0;
    uint16_t manualScale = 0;  // percent, 0 means the recommended one
};

struct FrameHeader {
    uint32_t frameId = 0;
    uint64_t captureUs = 0;  // host monotonic clock
    bool keyframe = false;
};

// Client -> host, about every 100 ms.
struct Stats {
    uint32_t windowMs = 0;
    uint32_t bytes = 0;
    uint32_t frames = 0;
    uint32_t packetsReceived = 0;
    uint32_t packetsLost = 0;       // never arrived, before FEC and NACK
    uint32_t framesDropped = 0;     // unrecoverable
    int32_t delayGradientUs = 0;    // growth of one-way delay across the window
    uint32_t decodeUs = 0;          // decoder input to output, average
    uint32_t rttUs = 0;
};

enum class TouchAction : uint8_t { Down = 0, Move = 1, Up = 2, Cancel = 3 };

struct TouchPoint {
    uint8_t id = 0;
    TouchAction action = TouchAction::Move;
    uint16_t x = 0;         // 0..65535 across the display width
    uint16_t y = 0;         // 0..65535 across the display height
    uint16_t pressure = 0;  // 0..1024
};

constexpr uint32_t kKeepBitrate = 0xFFFFFFFF;
constexpr uint8_t kMirrorOff = 0, kMirrorOn = 1, kMirrorKeep = 2;
constexpr uint16_t kKeepScale = 0xFFFF;
// In place of a width and height: use the size of the PC's main display.
constexpr uint16_t kResolutionMatchPc = 0xFFFF;

// Either side asks for a display mode. width/height 0 means automatic.
struct SetDisplay {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hz = 0;
    uint32_t maxBitrateKbps = 0;  // 0 means automatic, kKeepBitrate leaves it alone
    uint8_t mirror = kMirrorKeep;
    uint16_t scale = kKeepScale;  // percent, 0 means the recommended one
};

struct Size {
    uint16_t width = 0;
    uint16_t height = 0;
    bool operator==(const Size& o) const { return width == o.width && height == o.height; }
};

// Host -> client: what the display settings can be set to right now.
struct DisplayOptions {
    Size phone;
    Size pc;
    uint16_t recommendedScale = 100;
    uint16_t currentScale = 100;
    std::vector<Size> resolutions;
};

// Client -> host: orientation or window size changed.
struct Viewport {
    uint16_t width = 0;
    uint16_t height = 0;
};

struct Nack {
    uint32_t frameId = 0;
    struct Item { uint16_t block; uint8_t shard; };
    std::vector<Item> items;
};

// Host -> client: the UDP port to use, with keys derived from the handshake.
struct UdpSetup {
    uint16_t port = 0;
};

Bytes encode(const Hello&);
Bytes encode(const Config&);
Bytes encodeFrame(const FrameHeader&, const uint8_t* data, size_t size);
Bytes encode(const Stats&);
Bytes encodeTouch(const std::vector<TouchPoint>&);
Bytes encode(const SetDisplay&);
Bytes encode(const Viewport&);
Bytes encode(const Nack&);
Bytes encode(const UdpSetup&);
Bytes encode(const DisplayOptions&);
Bytes encodePing(MsgType pingOrPong, uint64_t timeUs);
Bytes encodeSimple(MsgType);

// `in` must start after the type byte.
std::optional<Hello> decodeHello(Reader in);
std::optional<Config> decodeConfig(Reader in);
std::optional<Stats> decodeStats(Reader in);
std::optional<std::vector<TouchPoint>> decodeTouch(Reader in);
std::optional<SetDisplay> decodeSetDisplay(Reader in);
std::optional<Viewport> decodeViewport(Reader in);
std::optional<Nack> decodeNack(Reader in);
std::optional<UdpSetup> decodeUdpSetup(Reader in);
std::optional<DisplayOptions> decodeDisplayOptions(Reader in);
// On success `payload`/`payloadSize` point into the buffer behind `in`.
bool decodeFrame(Reader in, FrameHeader& hdr, const uint8_t*& payload, size_t& payloadSize);

}  // namespace sd
