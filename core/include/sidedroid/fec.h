// Reed-Solomon erasure coding over GF(256) with a Cauchy matrix, and the
// splitting of frames into datagrams with reassembly on the other side.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "sidedroid/wire.h"

namespace sd {

namespace rs {
// shards[0..k) are data, shards[k..k+m) receive parity. All have equal size.
void encode(int k, int m, const std::vector<const uint8_t*>& data, const std::vector<uint8_t*>& parity, size_t size);
// present[i] says whether shards[i] (i in 0..k+m) holds valid data. Missing data
// shards are rebuilt in place. Returns false if fewer than k shards are present.
bool reconstruct(int k, int m, const std::vector<uint8_t*>& shards, const std::vector<bool>& present, size_t size);
}  // namespace rs

constexpr size_t kShardPayload = 1180;
constexpr int kBlockDataShards = 32;
constexpr size_t kPacketHeader = 18;

struct PacketHeader {
    uint32_t frameId;
    uint32_t frameBytes;
    uint16_t block;
    uint16_t blockCount;
    uint8_t shard;
    uint8_t dataShards;
    uint8_t parityShards;
    uint8_t flags;  // 1 = retransmission
    uint16_t reserved;
};

void writePacketHeader(const PacketHeader&, uint8_t* out);
bool readPacketHeader(const uint8_t* in, size_t size, PacketHeader& out);

// Splits one message into header-prefixed packets. parityPercent is applied per block.
std::vector<Bytes> packetize(uint32_t frameId, const uint8_t* data, size_t size, int parityPercent);

// Receiver side. Feed every packet, then call poll() regularly.
// Delivers frames in order. A frame still incomplete after dropAfterUs is
// abandoned and reported through onDrop.
class FrameAssembler {
public:
    struct Callbacks {
        std::function<void(uint32_t frameId, Bytes&& data)> onFrame;
        std::function<void(const Nack&)> onNack;
        std::function<void(uint32_t frameId)> onDrop;
    };
    struct Tuning {
        uint64_t nackAfterUs = 8000;
        uint64_t nackRepeatUs = 20000;
        uint64_t dropAfterUs = 120000;
    };

    explicit FrameAssembler(Callbacks cb) : cb_(std::move(cb)) {}
    FrameAssembler(Callbacks cb, Tuning t) : cb_(std::move(cb)), tuning_(t) {}

    void onPacket(const uint8_t* packet, size_t size, uint64_t nowUs);
    void poll(uint64_t nowUs);

    uint64_t packetsReceived() const { return received_; }

private:
    struct Block {
        int data = 0, parity = 0;
        std::vector<Bytes> shards;
        std::vector<bool> present;
        int have = 0;
        bool done = false;
    };
    struct Partial {
        uint32_t bytes = 0;
        uint64_t firstUs = 0, lastNackUs = 0;
        std::vector<Block> blocks;
        int blocksDone = 0;
        bool complete = false;
    };
    void deliverReady(uint64_t nowUs);
    static void finishBlock(Block& b);

    Callbacks cb_;
    Tuning tuning_;
    std::map<uint32_t, Partial> frames_;
    uint32_t next_ = 0;
    bool started_ = false;
    uint64_t received_ = 0;
};

// Sender keeps recent packets so that NACKed shards can be sent again.
class RetransmitBuffer {
public:
    explicit RetransmitBuffer(size_t maxFrames = 32) : maxFrames_(maxFrames) {}
    void store(uint32_t frameId, const std::vector<Bytes>& packets);
    // Returns the packet flagged as retransmission, or an empty vector.
    Bytes find(uint32_t frameId, uint16_t block, uint8_t shard) const;

private:
    size_t maxFrames_;
    std::map<uint32_t, std::vector<Bytes>> frames_;
};

}  // namespace sd
