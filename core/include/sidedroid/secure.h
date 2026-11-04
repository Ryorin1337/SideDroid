// Authenticated encrypted channel, built on Monocypher (X25519, BLAKE2b,
// XChaCha20-Poly1305, Argon2id). See docs/protocol.md.
//
// The handshake mixes an ephemeral X25519 secret with a pre-shared key:
//   Usb     : psk is all zero. The host accepts this only from loopback, which
//             is where adb-reverse connections appear.
//   Pairing : psk = Argon2id(one-time pairing code, salt = both public keys).
//   Paired  : psk = the secret saved by an earlier pairing, found by client id.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "sidedroid/net.h"
#include "sidedroid/wire.h"

namespace sd {

using Key32 = std::array<uint8_t, 32>;
using ClientId = std::array<uint8_t, 16>;

enum class AuthMode : uint8_t { Usb = 0, Pairing = 1, Paired = 2 };

// Fills with OS randomness. Returns false if the OS refused.
bool secureRandom(void* out, size_t n);

Key32 pskFromPairingCode(const std::string& code, const uint8_t clientPub[32], const uint8_t hostPub[32]);

// Generates a one-time pairing code like "K7QM-2XPD" (40 bits, Crockford base32).
std::string generatePairingCode();
// Removes separators and folds case and look-alike characters.
std::string normalizePairingCode(const std::string& code);

struct SessionKeys {
    Key32 tcpC2H{};   // client to host
    Key32 tcpH2C{};
    Key32 udpH2C{};
    Key32 pairSecret{};  // saved by both sides when mode is Pairing
};

// One encrypted, length-prefixed record stream over a TCP socket. Send and
// receive may run on different threads; send is serialized internally.
class SecureChannel {
public:
    SecureChannel() = default;
    SecureChannel(Socket&& s, const Key32& sendKey, const Key32& recvKey);

    bool send(const Bytes& plain);
    bool send(const uint8_t* plain, size_t size);
    bool recv(Bytes& plain);  // false on EOF, tampering or oversize record
    void shutdown() { sock_.shutdownBoth(); }
    Socket& socket() { return sock_; }

private:
    static constexpr uint32_t kMaxRecord = 32u * 1024u * 1024u;
    Socket sock_;
    Key32 sendKey_{}, recvKey_{};
    uint64_t sendSeq_ = 0, recvSeq_ = 0;
    std::mutex sendMutex_;
    Bytes sendBuf_, recvBuf_;
};

struct HandshakeResult {
    SessionKeys keys;
    AuthMode mode = AuthMode::Usb;
    ClientId clientId{};
};

// Host side lookups. Both are only consulted for the mode the client asked for.
struct HostAuth {
    bool peerIsLoopback = false;
    std::function<std::optional<std::string>()> currentPairingCode;
    std::function<std::optional<Key32>(const ClientId&)> lookupPaired;
};

// Both run on a blocking socket. On success the socket is still connected.
std::optional<HandshakeResult> handshakeClient(Socket& s, AuthMode mode, const ClientId& id,
                                               const Key32& psk,
                                               const std::function<Key32(const uint8_t*, const uint8_t*)>& pskFromPubs = {});
std::optional<HandshakeResult> handshakeHost(Socket& s, const HostAuth& auth);

// Per-packet AEAD for media datagrams. Nonce comes from a 64-bit sequence
// number carried in the clear, and the receiver drops replays with a window.
constexpr size_t kUdpOverhead = 8 + 16;  // seq + MAC
void udpSeal(const Key32& key, uint64_t seq, const uint8_t* plain, size_t size, uint8_t* out);
bool udpOpen(const Key32& key, const uint8_t* packet, size_t size, uint8_t* plainOut, uint64_t* seqOut);

class ReplayWindow {
public:
    bool accept(uint64_t seq);

private:
    uint64_t top_ = 0;
    uint64_t bits_ = 0;
    bool any_ = false;
};

}  // namespace sd
