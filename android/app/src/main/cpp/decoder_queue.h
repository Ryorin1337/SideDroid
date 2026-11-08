#pragma once

#include <algorithm>
#include <deque>
#include <optional>

#include "sidedroid/wire.h"

namespace sd {

struct DecoderFrame {
    FrameHeader header;
    Bytes data;
    uint64_t receivedUs;
};

// Caller supplies synchronization. Dropping encoded data invalidates its entire reference chain.
class DecoderQueue {
public:
    void configure(int fps) {
        maxAgeUs_ = std::clamp<uint64_t>(2000000 / std::max(fps, 1), 16000, 50000);
        reset();
    }

    void reset() {
        frames_.clear();
        awaitingKey_ = true;
        ++generation_;
    }

    void push(const FrameHeader& h, const uint8_t* data, size_t size, uint64_t nowUs) {
        if (frames_.size() >= kMaxFrames || expired(nowUs)) drop();
        if (awaitingKey_ && !h.keyframe) return;
        awaitingKey_ = false;
        frames_.push_back({h, Bytes(data, data + size), nowUs});
    }

    std::optional<DecoderFrame> take(uint64_t nowUs) {
        if (expired(nowUs)) drop();
        if (frames_.empty()) return std::nullopt;
        DecoderFrame f = std::move(frames_.front());
        frames_.pop_front();
        return f;
    }

    bool empty() const { return frames_.empty(); }
    bool awaitingKey() const { return awaitingKey_; }
    uint64_t generation() const { return generation_; }
    uint64_t maxAgeUs() const { return maxAgeUs_; }
    uint64_t drops() const { return drops_; }

    void drop() {
        reset();
        ++drops_;
    }

private:
    bool expired(uint64_t nowUs) const {
        return !frames_.empty() && nowUs - frames_.front().receivedUs >= maxAgeUs_;
    }

    static constexpr size_t kMaxFrames = 3;
    std::deque<DecoderFrame> frames_;
    uint64_t generation_ = 0, maxAgeUs_ = 33333;
    uint64_t drops_ = 0;
    bool awaitingKey_ = true;
};

}  // namespace sd
