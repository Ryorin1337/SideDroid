// Decides bitrate, frame rate and encode scale from receiver feedback.
// Pure logic: no clock, no I/O.
#pragma once
#include <cstdint>

#include "sidedroid/wire.h"

namespace sd {

struct RateTarget {
    uint32_t bitrateKbps = 0;
    int fps = 0;
    int scalePercent = 100;   // of the display resolution
    int parityPercent = 0;    // FEC strength, UDP only
    bool operator==(const RateTarget&) const = default;
};

struct RateLimits {
    uint32_t minKbps = 2000;
    uint32_t maxKbps = 40000;
    uint32_t startKbps = 12000;
    int maxFps = 60;
    int minFps = 30;
    bool udp = false;
};

class RateController {
public:
    explicit RateController(const RateLimits& limits);

    // Host-side backlog (TCP send queue depth or similar) counts as congestion.
    RateTarget update(const Stats& s, double senderBacklogMs);
    const RateTarget& target() const { return target_; }
    void setLimits(const RateLimits& limits);

private:
    enum class Signal { Overuse, Hold, Underuse };
    Signal classify(const Stats& s, double backlogMs);

    RateLimits limits_;
    RateTarget target_;
    double queueUs_ = 0;      // estimated standing queue
    double cumDelayUs_ = 0;
    double baseDelayUs_ = 0;
    double lossEma_ = 0;
    int cleanWindows_ = 0;
    int overuseWindows_ = 0;
    int cooldown_ = 0;
};

}  // namespace sd
