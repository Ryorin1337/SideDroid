#include "sidedroid/secure.h"

#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "monocypher.h"
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <cstdio>
#endif

namespace sd {

bool secureRandom(void* out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t r = std::fread(out, 1, n, f);
    std::fclose(f);
    return r == n;
#endif
}

static const char kBase32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string generatePairingCode() {
    uint8_t r[8];
    if (!secureRandom(r, sizeof(r))) std::abort();  // never fall back to weak randomness
    std::string s;
    for (int i = 0; i < 8; ++i) {
        if (i == 4) s.push_back('-');
        s.push_back(kBase32[r[i] & 31]);
    }
    crypto_wipe(r, sizeof(r));
    return s;
}

std::string normalizePairingCode(const std::string& code) {
    std::string s;
    for (char c : code) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        if (std::strchr(kBase32, c) && c != 0) s.push_back(c);
    }
    return s;
}

Key32 pskFromPairingCode(const std::string& code, const uint8_t clientPub[32], const uint8_t hostPub[32]) {
    std::string norm = normalizePairingCode(code);
    uint8_t salt[32];
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, sizeof(salt));
    crypto_blake2b_update(&ctx, clientPub, 32);
    crypto_blake2b_update(&ctx, hostPub, 32);
    crypto_blake2b_final(&ctx, salt);

    // Stretching makes guessing costly while the two-minute code is alive.
    crypto_argon2_config cfg{};
    cfg.algorithm = CRYPTO_ARGON2_ID;
    cfg.nb_blocks = 16 * 1024;  // 16 MiB
    cfg.nb_passes = 2;
    cfg.nb_lanes = 1;
    crypto_argon2_inputs in{};
    in.pass = reinterpret_cast<const uint8_t*>(norm.data());
    in.pass_size = static_cast<uint32_t>(norm.size());
    in.salt = salt;
    in.salt_size = sizeof(salt);
    std::vector<uint8_t> work(static_cast<size_t>(cfg.nb_blocks) * 1024);
    Key32 out{};
    crypto_argon2(out.data(), 32, work.data(), cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work.data(), work.size());
    return out;
}

static void makeNonce(uint8_t nonce[24], uint64_t seq, uint8_t domain) {
    std::memset(nonce, 0, 24);
    nonce[0] = domain;
    for (int i = 0; i < 8; ++i) nonce[16 + i] = static_cast<uint8_t>(seq >> (8 * i));
}

SecureChannel::SecureChannel(Socket&& s, const Key32& sendKey, const Key32& recvKey)
    : sock_(std::move(s)), sendKey_(sendKey), recvKey_(recvKey) {}

bool SecureChannel::send(const Bytes& plain) { return send(plain.data(), plain.size()); }

bool SecureChannel::send(const uint8_t* plain, size_t size) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (size > kMaxRecord) return false;
    sendBuf_.resize(4 + 16 + size);
    auto n = static_cast<uint32_t>(size);
    for (int i = 0; i < 4; ++i) sendBuf_[i] = static_cast<uint8_t>(n >> (8 * i));
    uint8_t nonce[24];
    makeNonce(nonce, sendSeq_++, 1);
    crypto_aead_lock(sendBuf_.data() + 20, sendBuf_.data() + 4, sendKey_.data(), nonce,
                     sendBuf_.data(), 4, plain, size);
    return sock_.sendAll(sendBuf_.data(), sendBuf_.size());
}

bool SecureChannel::recv(Bytes& plain) {
    uint8_t hdr[4];
    if (!sock_.recvAll(hdr, 4)) return false;
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= static_cast<uint32_t>(hdr[i]) << (8 * i);
    if (n > kMaxRecord) return false;
    recvBuf_.resize(16 + n);
    if (!sock_.recvAll(recvBuf_.data(), recvBuf_.size())) return false;
    plain.resize(n);
    uint8_t nonce[24];
    makeNonce(nonce, recvSeq_++, 1);
    return crypto_aead_unlock(plain.data(), recvBuf_.data(), recvKey_.data(), nonce, hdr, 4,
                              recvBuf_.data() + 16, n) == 0;
}

namespace {

constexpr uint8_t kMagic[4] = {'S', 'D', 'H', '1'};

struct Derived {
    Key32 confirmClient, confirmHost;
    SessionKeys keys;
};

// transcript = BLAKE2b(magic || mode || id || clientPub || hostPub)
Derived deriveKeys(const uint8_t shared[32], const Key32& psk, AuthMode mode, const ClientId& id,
                   const uint8_t clientPub[32], const uint8_t hostPub[32]) {
    uint8_t transcript[64];
    crypto_blake2b_ctx t;
    crypto_blake2b_init(&t, 64);
    crypto_blake2b_update(&t, kMagic, 4);
    uint8_t m = static_cast<uint8_t>(mode);
    crypto_blake2b_update(&t, &m, 1);
    crypto_blake2b_update(&t, id.data(), id.size());
    crypto_blake2b_update(&t, clientPub, 32);
    crypto_blake2b_update(&t, hostPub, 32);
    crypto_blake2b_final(&t, transcript);

    uint8_t master[64];
    crypto_blake2b_ctx mctx;
    crypto_blake2b_keyed_init(&mctx, 64, psk.data(), psk.size());
    crypto_blake2b_update(&mctx, shared, 32);
    crypto_blake2b_update(&mctx, transcript, sizeof(transcript));
    crypto_blake2b_final(&mctx, master);

    auto expand = [&](const char* label, Key32& out) {
        crypto_blake2b_keyed(out.data(), 32, master, 64,
                             reinterpret_cast<const uint8_t*>(label), std::strlen(label));
    };
    Derived d;
    expand("confirm client", d.confirmClient);
    expand("confirm host", d.confirmHost);
    expand("tcp c2h", d.keys.tcpC2H);
    expand("tcp h2c", d.keys.tcpH2C);
    expand("udp h2c", d.keys.udpH2C);
    expand("pair secret", d.keys.pairSecret);
    crypto_wipe(master, sizeof(master));
    return d;
}

}  // namespace

std::optional<HandshakeResult> handshakeClient(
    Socket& s, AuthMode mode, const ClientId& id, const Key32& psk,
    const std::function<Key32(const uint8_t*, const uint8_t*)>& pskFromPubs) {
    uint8_t sk[32], pk[32], hostPk[32], shared[32];
    if (!secureRandom(sk, 32)) return std::nullopt;
    crypto_x25519_public_key(pk, sk);

    Bytes hello;
    Writer w(hello);
    w.raw(kMagic, 4);
    w.u8(static_cast<uint8_t>(mode));
    w.raw(id.data(), id.size());
    w.raw(pk, 32);
    if (!s.sendAll(hello.data(), hello.size())) return std::nullopt;
    if (!s.recvAll(hostPk, 32)) return std::nullopt;

    crypto_x25519(shared, sk, hostPk);
    Key32 effective = pskFromPubs ? pskFromPubs(pk, hostPk) : psk;
    Derived d = deriveKeys(shared, effective, mode, id, pk, hostPk);
    crypto_wipe(sk, 32);
    crypto_wipe(shared, 32);

    if (!s.sendAll(d.confirmClient.data(), 32)) return std::nullopt;
    uint8_t got[32];
    if (!s.recvAll(got, 32)) return std::nullopt;
    if (crypto_verify32(got, d.confirmHost.data()) != 0) return std::nullopt;

    HandshakeResult r;
    r.keys = d.keys;
    r.mode = mode;
    r.clientId = id;
    return r;
}

std::optional<HandshakeResult> handshakeHost(Socket& s, const HostAuth& auth) {
    uint8_t buf[4 + 1 + 16 + 32];
    if (!s.recvAll(buf, sizeof(buf))) return std::nullopt;
    if (std::memcmp(buf, kMagic, 4) != 0 || buf[4] > 2) return std::nullopt;
    auto mode = static_cast<AuthMode>(buf[4]);
    ClientId id{};
    std::memcpy(id.data(), buf + 5, id.size());
    const uint8_t* clientPk = buf + 21;

    uint8_t sk[32], pk[32], shared[32];
    if (!secureRandom(sk, 32)) return std::nullopt;
    crypto_x25519_public_key(pk, sk);
    crypto_x25519(shared, sk, clientPk);

    Key32 psk{};
    switch (mode) {
    case AuthMode::Usb:
        if (!auth.peerIsLoopback) return std::nullopt;
        break;
    case AuthMode::Pairing: {
        auto code = auth.currentPairingCode ? auth.currentPairingCode() : std::nullopt;
        if (!code) return std::nullopt;
        psk = pskFromPairingCode(*code, clientPk, pk);
        break;
    }
    case AuthMode::Paired: {
        auto k = auth.lookupPaired ? auth.lookupPaired(id) : std::nullopt;
        if (!k) return std::nullopt;
        psk = *k;
        break;
    }
    }

    if (!s.sendAll(pk, 32)) return std::nullopt;
    Derived d = deriveKeys(shared, psk, mode, id, clientPk, pk);
    crypto_wipe(sk, 32);
    crypto_wipe(shared, 32);

    // The client proves knowledge first, so a wrong code learns nothing from us.
    uint8_t got[32];
    if (!s.recvAll(got, 32)) return std::nullopt;
    if (crypto_verify32(got, d.confirmClient.data()) != 0) return std::nullopt;
    if (!s.sendAll(d.confirmHost.data(), 32)) return std::nullopt;

    HandshakeResult r;
    r.keys = d.keys;
    r.mode = mode;
    r.clientId = id;
    return r;
}

void udpSeal(const Key32& key, uint64_t seq, const uint8_t* plain, size_t size, uint8_t* out) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(seq >> (8 * i));
    uint8_t nonce[24];
    makeNonce(nonce, seq, 2);
    crypto_aead_lock(out + kUdpOverhead, out + 8, key.data(), nonce, out, 8, plain, size);
}

bool udpOpen(const Key32& key, const uint8_t* p, size_t size, uint8_t* plainOut, uint64_t* seqOut) {
    if (size < kUdpOverhead) return false;
    uint64_t seq = 0;
    for (int i = 0; i < 8; ++i) seq |= static_cast<uint64_t>(p[i]) << (8 * i);
    uint8_t nonce[24];
    makeNonce(nonce, seq, 2);
    if (crypto_aead_unlock(plainOut, p + 8, key.data(), nonce, p, 8, p + kUdpOverhead,
                           size - kUdpOverhead) != 0)
        return false;
    *seqOut = seq;
    return true;
}

bool ReplayWindow::accept(uint64_t seq) {
    if (!any_) {
        any_ = true;
        top_ = seq;
        bits_ = 1;
        return true;
    }
    if (seq > top_) {
        uint64_t shift = seq - top_;
        bits_ = shift >= 64 ? 0 : bits_ << shift;
        bits_ |= 1;
        top_ = seq;
        return true;
    }
    uint64_t back = top_ - seq;
    if (back >= 64) return false;
    uint64_t mask = 1ull << back;
    if (bits_ & mask) return false;
    bits_ |= mask;
    return true;
}

}  // namespace sd
