#include "sidedroid/fec.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace sd {

namespace {

constexpr uint32_t kMaxFramesAhead = 1024;

struct Gf {
    std::array<uint8_t, 512> exp{};
    std::array<uint8_t, 256> log{};
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
    uint8_t mul(uint8_t a, uint8_t b) const {
        return (a == 0 || b == 0) ? 0 : exp[log[a] + log[b]];
    }
    uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
};

const Gf& gf() {
    static const Gf g;
    return g;
}

// Cauchy element for parity row r over data column c.
uint8_t cauchy(int k, int r, int c) {
    return gf().inv(static_cast<uint8_t>((k + r) ^ c));
}

void mulAdd(uint8_t* dst, const uint8_t* src, uint8_t coef, size_t n) {
    if (coef == 0) return;
    const auto& g = gf();
    const int lc = g.log[coef];
    for (size_t i = 0; i < n; ++i)
        if (src[i]) dst[i] ^= g.exp[g.log[src[i]] + lc];
}

}  // namespace

namespace rs {

void encode(int k, int m, const std::vector<const uint8_t*>& data, const std::vector<uint8_t*>& parity, size_t size) {
    for (int r = 0; r < m; ++r) {
        std::memset(parity[r], 0, size);
        for (int c = 0; c < k; ++c) mulAdd(parity[r], data[c], cauchy(k, r, c), size);
    }
}

bool reconstruct(int k, int m, const std::vector<uint8_t*>& shards, const std::vector<bool>& present, size_t size) {
    std::vector<int> missing;
    for (int i = 0; i < k; ++i)
        if (!present[i]) missing.push_back(i);
    if (missing.empty()) return true;

    std::vector<int> parityRows;
    for (int r = 0; r < m && parityRows.size() < missing.size(); ++r)
        if (present[k + r]) parityRows.push_back(r);
    if (parityRows.size() < missing.size()) return false;

    // Build syndromes: each used parity minus the contribution of known data.
    const int n = static_cast<int>(missing.size());
    std::vector<Bytes> syn(n, Bytes(size));
    for (int i = 0; i < n; ++i) {
        std::memcpy(syn[i].data(), shards[k + parityRows[i]], size);
        for (int c = 0; c < k; ++c)
            if (present[c]) mulAdd(syn[i].data(), shards[c], cauchy(k, parityRows[i], c), size);
    }

    // Gauss-Jordan over the square matrix for the missing columns.
    std::vector<std::vector<uint8_t>> a(n, std::vector<uint8_t>(n));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) a[i][j] = cauchy(k, parityRows[i], missing[j]);

    const auto& g = gf();
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        while (pivot < n && a[pivot][col] == 0) ++pivot;
        if (pivot == n) return false;
        std::swap(a[pivot], a[col]);
        std::swap(syn[pivot], syn[col]);
        uint8_t iv = g.inv(a[col][col]);
        for (int j = 0; j < n; ++j) a[col][j] = g.mul(a[col][j], iv);
        for (size_t b = 0; b < size; ++b) syn[col][b] = g.mul(syn[col][b], iv);
        for (int row = 0; row < n; ++row) {
            if (row == col || a[row][col] == 0) continue;
            uint8_t f = a[row][col];
            for (int j = 0; j < n; ++j) a[row][j] ^= g.mul(f, a[col][j]);
            mulAdd(syn[row].data(), syn[col].data(), f, size);
        }
    }
    for (int i = 0; i < n; ++i) std::memcpy(shards[missing[i]], syn[i].data(), size);
    return true;
}

}  // namespace rs

void writePacketHeader(const PacketHeader& h, uint8_t* out) {
    Bytes b;
    Writer w(b);
    w.u32(h.frameId);
    w.u32(h.frameBytes);
    w.u16(h.block);
    w.u16(h.blockCount);
    w.u8(h.shard);
    w.u8(h.dataShards);
    w.u8(h.parityShards);
    w.u8(h.flags);
    w.u16(h.reserved);
    std::memcpy(out, b.data(), kPacketHeader);
}

bool readPacketHeader(const uint8_t* in, size_t size, PacketHeader& h) {
    if (size < kPacketHeader) return false;
    Reader r(in, kPacketHeader);
    h.frameId = r.u32();
    h.frameBytes = r.u32();
    h.block = r.u16();
    h.blockCount = r.u16();
    h.shard = r.u8();
    h.dataShards = r.u8();
    h.parityShards = r.u8();
    h.flags = r.u8();
    h.reserved = r.u16();
    return r.ok();
}

std::vector<Bytes> packetize(uint32_t frameId, const uint8_t* data, size_t size, int parityPercent) {
    const size_t blockBytes = kShardPayload * kBlockDataShards;
    const int blockCount = static_cast<int>(std::max<size_t>(1, (size + blockBytes - 1) / blockBytes));
    std::vector<Bytes> out;

    for (int b = 0; b < blockCount; ++b) {
        size_t offset = static_cast<size_t>(b) * blockBytes;
        size_t chunk = std::min(blockBytes, size - std::min(size, offset));
        int k = static_cast<int>(std::max<size_t>(1, (chunk + kShardPayload - 1) / kShardPayload));
        // At least one parity shard whenever FEC is on, so that small frames gain too.
        int m = parityPercent <= 0 ? 0 : std::max(1, (k * parityPercent + 99) / 100);

        std::vector<Bytes> shards(k + m, Bytes(kPacketHeader + kShardPayload, 0));
        std::vector<const uint8_t*> dataPtrs;
        std::vector<uint8_t*> parityPtrs;
        for (int i = 0; i < k; ++i) {
            size_t off = offset + static_cast<size_t>(i) * kShardPayload;
            size_t n = std::min(kShardPayload, size - std::min(size, off));
            if (n) std::memcpy(shards[i].data() + kPacketHeader, data + off, n);
            dataPtrs.push_back(shards[i].data() + kPacketHeader);
        }
        for (int i = 0; i < m; ++i) parityPtrs.push_back(shards[k + i].data() + kPacketHeader);
        if (m) rs::encode(k, m, dataPtrs, parityPtrs, kShardPayload);

        for (int i = 0; i < k + m; ++i) {
            PacketHeader h{frameId, static_cast<uint32_t>(size), static_cast<uint16_t>(b),
                           static_cast<uint16_t>(blockCount), static_cast<uint8_t>(i),
                           static_cast<uint8_t>(k), static_cast<uint8_t>(m), 0, 0};
            writePacketHeader(h, shards[i].data());
            out.push_back(std::move(shards[i]));
        }
    }
    return out;
}

void FrameAssembler::finishBlock(Block& b) {
    std::vector<uint8_t*> ptrs;
    for (auto& s : b.shards) ptrs.push_back(s.data());
    rs::reconstruct(b.data, b.parity, ptrs, b.present, kShardPayload);
    b.done = true;
}

void FrameAssembler::onPacket(const uint8_t* packet, size_t size, uint64_t nowUs) {
    PacketHeader h;
    if (!readPacketHeader(packet, size, h)) return;
    if (size != kPacketHeader + kShardPayload) return;
    if (h.dataShards == 0 || h.dataShards > kBlockDataShards || h.parityShards > kBlockDataShards) return;
    if (h.shard >= static_cast<int>(h.dataShards) + h.parityShards) return;
    if (h.blockCount == 0 || h.block >= h.blockCount) return;
    // Reject sizes that the claimed block layout could not carry.
    const uint64_t blockBytes = static_cast<uint64_t>(kBlockDataShards) * kShardPayload;
    const uint64_t expectedBlocks = std::max<uint64_t>(1, (h.frameBytes + blockBytes - 1) / blockBytes);
    if (h.blockCount != expectedBlocks || h.frameBytes > 32u * 1024u * 1024u) return;
    ++received_;

    if (!started_) {
        started_ = true;
        next_ = h.frameId;
    }
    if (static_cast<int32_t>(h.frameId - next_) < 0) return;  // late
    if (h.frameId - next_ > kMaxFramesAhead) return;           // not a plausible stream position

    auto& f = frames_[h.frameId];
    if (f.blocks.empty()) {
        f.bytes = h.frameBytes;
        f.firstUs = nowUs;
        f.blocks.resize(h.blockCount);
    }
    if (f.complete || h.block >= f.blocks.size() || h.frameBytes != f.bytes) return;

    Block& b = f.blocks[h.block];
    if (b.shards.empty()) {
        b.data = h.dataShards;
        b.parity = h.parityShards;
        b.shards.assign(b.data + b.parity, Bytes(kShardPayload));
        b.present.assign(b.data + b.parity, false);
    }
    if (b.done || h.dataShards != b.data || h.parityShards != b.parity || b.present[h.shard]) return;
    std::memcpy(b.shards[h.shard].data(), packet + kPacketHeader, kShardPayload);
    b.present[h.shard] = true;
    ++b.have;
    if (b.have >= b.data) {
        finishBlock(b);
        ++f.blocksDone;
        if (f.blocksDone == static_cast<int>(f.blocks.size())) f.complete = true;
    }
    deliverReady(nowUs);
}

void FrameAssembler::deliverReady(uint64_t nowUs) {
    (void)nowUs;
    while (true) {
        auto it = frames_.find(next_);
        if (it == frames_.end() || !it->second.complete) return;
        Partial& f = it->second;
        Bytes out;
        out.reserve(f.bytes);
        for (auto& b : f.blocks)
            for (int i = 0; i < b.data; ++i)
                out.insert(out.end(), b.shards[i].begin(), b.shards[i].end());
        out.resize(f.bytes);
        uint32_t id = it->first;
        frames_.erase(it);
        ++next_;
        if (cb_.onFrame) cb_.onFrame(id, std::move(out));
    }
}

void FrameAssembler::poll(uint64_t nowUs) {
    // Drop anything that has waited too long, oldest first.
    while (!frames_.empty()) {
        auto it = frames_.begin();
        // Advance past frames that no packet ever arrived for once a newer one is overdue.
        if (it->second.complete || nowUs - it->second.firstUs < tuning_.dropAfterUs) break;
        uint32_t id = it->first;
        frames_.erase(it);
        if (static_cast<int32_t>(id - next_) >= 0) {
            for (uint32_t skipped = next_; skipped != id + 1; ++skipped)
                if (cb_.onDrop) cb_.onDrop(skipped);
            next_ = id + 1;
        }
    }

    // A wholly lost frame leaves a gap below a newer complete one.
    if (!frames_.empty() && frames_.find(next_) == frames_.end()) {
        auto first = frames_.begin();
        const bool ahead = static_cast<int32_t>(first->first - next_) > 0;
        if (ahead && nowUs - first->second.firstUs >= tuning_.nackAfterUs * 3) {
            for (uint32_t skipped = next_; skipped != first->first; ++skipped)
                if (cb_.onDrop) cb_.onDrop(skipped);
            next_ = first->first;
        }
    }
    deliverReady(nowUs);

    for (auto& [id, f] : frames_) {
        if (f.complete) continue;
        if (nowUs - f.firstUs < tuning_.nackAfterUs) continue;
        if (f.lastNackUs != 0 && nowUs - f.lastNackUs < tuning_.nackRepeatUs) continue;
        Nack n;
        n.frameId = id;
        for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
            const Block& b = f.blocks[bi];
            if (b.done) continue;
            if (b.shards.empty()) {
                // Nothing known about this block yet; ask for its first shards.
                for (int s = 0; s < kBlockDataShards; ++s)
                    n.items.push_back({static_cast<uint16_t>(bi), static_cast<uint8_t>(s)});
                continue;
            }
            int need = b.data - b.have;
            for (int s = 0; s < b.data + b.parity && need > 0; ++s) {
                if (!b.present[s]) {
                    n.items.push_back({static_cast<uint16_t>(bi), static_cast<uint8_t>(s)});
                    --need;
                }
            }
        }
        if (!n.items.empty()) {
            f.lastNackUs = nowUs;
            if (cb_.onNack) cb_.onNack(n);
        }
    }
}

void RetransmitBuffer::store(uint32_t frameId, const std::vector<Bytes>& packets) {
    frames_[frameId] = packets;
    while (frames_.size() > maxFrames_) frames_.erase(frames_.begin());
}

Bytes RetransmitBuffer::find(uint32_t frameId, uint16_t block, uint8_t shard) const {
    auto it = frames_.find(frameId);
    if (it == frames_.end()) return {};
    for (const auto& p : it->second) {
        PacketHeader h;
        if (readPacketHeader(p.data(), p.size(), h) && h.block == block && h.shard == shard) {
            Bytes copy = p;
            h.flags |= 1;
            writePacketHeader(h, copy.data());
            return copy;
        }
    }
    return {};
}

}  // namespace sd
