#include "sidedroid/ratecontrol.h"

#include <algorithm>

namespace sd {

namespace {
constexpr int kScaleLadder[] = {100, 85, 70, 50};
constexpr double kOveruseQueueUs = 25000;    // a standing queue of 25 ms
constexpr double kUnderuseQueueUs = 8000;
constexpr double kBaseForgetUs = 50;         // per window, so an old minimum cannot pin the estimate
// Below this, random radio loss is FEC's job, not a reason to send less.
constexpr double kOveruseLoss = 0.10;
constexpr double kOveruseBacklogMs = 40;
constexpr int kWindowsBeforeUpgrade = 30;    // about 3 s clean before raising fps or scale

int fecForLoss(double loss) {
    if (loss < 0.002) return 5;
    if (loss < 0.01) return 10;
    if (loss < 0.05) return 20;
    return 30;
}
}  // namespace

RateController::RateController(const RateLimits& limits) { setLimits(limits); target_.bitrateKbps = std::clamp(limits.startKbps, limits.minKbps, limits.maxKbps); }

void RateController::setLimits(const RateLimits& limits) {
    limits_ = limits;
    target_.bitrateKbps = std::clamp(target_.bitrateKbps ? target_.bitrateKbps : limits.startKbps,
                                     limits.minKbps, limits.maxKbps);
    target_.fps = target_.fps ? std::min(target_.fps, limits.maxFps) : limits.maxFps;
    target_.parityPercent = limits.udp ? 10 : 0;
}

RateController::Signal RateController::classify(const Stats& s, double backlogMs) {
    double loss = 0;
    uint32_t total = s.packetsReceived + s.packetsLost;
    if (total > 0) loss = static_cast<double>(s.packetsLost) / total;
    lossEma_ = lossEma_ * 0.8 + loss * 0.2;

    // Single windows jitter by milliseconds. What matters is whether delay has
    // accumulated above the lowest level seen recently.
    cumDelayUs_ += s.delayGradientUs;
    baseDelayUs_ = std::min(baseDelayUs_ + kBaseForgetUs, cumDelayUs_);
    queueUs_ = cumDelayUs_ - baseDelayUs_;

    if (s.framesDropped > 0 || loss >= kOveruseLoss || queueUs_ > kOveruseQueueUs || backlogMs > kOveruseBacklogMs)
        return Signal::Overuse;
    if (queueUs_ < kUnderuseQueueUs && lossEma_ < 0.05 && backlogMs < 10) return Signal::Underuse;
    return Signal::Hold;
}

RateTarget RateController::update(const Stats& s, double backlogMs) {
    Signal sig = classify(s, backlogMs);
    if (limits_.udp) target_.parityPercent = fecForLoss(lossEma_);
    if (cooldown_ > 0) --cooldown_;

    switch (sig) {
    case Signal::Overuse:
        cleanWindows_ = 0;
        ++overuseWindows_;
        if (cooldown_ == 0) {
            target_.bitrateKbps = std::max(limits_.minKbps, static_cast<uint32_t>(target_.bitrateKbps * 0.8));
            cooldown_ = 3;  // give the reduced rate time to show up in feedback
        }
        // Only when the bitrate floor is not enough do we touch fps, then scale,
        // because those are the changes a user can actually see.
        if (target_.bitrateKbps <= limits_.minKbps && overuseWindows_ >= 10) {
            overuseWindows_ = 0;
            if (target_.fps > limits_.minFps) {
                target_.fps = std::max(limits_.minFps, target_.fps / 2);
            } else {
                for (int rung : kScaleLadder) {
                    if (rung < target_.scalePercent) {
                        target_.scalePercent = rung;
                        break;
                    }
                }
            }
        }
        break;
    case Signal::Hold:
        overuseWindows_ = 0;
        break;
    case Signal::Underuse:
        overuseWindows_ = 0;
        ++cleanWindows_;
        // Gentle probing upward, about 5% per second. An idle desktop sending far
        // less than the target proves nothing about the path, so hold there,
        // except to come back up to the normal starting rate.
        if (s.bytes * 8.0 / std::max<uint32_t>(s.windowMs, 1) > target_.bitrateKbps * 0.5 ||
            target_.bitrateKbps < limits_.startKbps) {
            target_.bitrateKbps = std::min(limits_.maxKbps, target_.bitrateKbps + std::max<uint32_t>(50, target_.bitrateKbps / 200));
        }
        if (cleanWindows_ >= kWindowsBeforeUpgrade) {
            cleanWindows_ = 0;
            // Upgrade in the reverse order, and only once bitrate has real headroom.
            if (target_.scalePercent < 100) {
                for (auto it = std::rbegin(kScaleLadder); it != std::rend(kScaleLadder); ++it) {
                    if (*it > target_.scalePercent) {
                        target_.scalePercent = *it;
                        break;
                    }
                }
            } else if (target_.fps < limits_.maxFps) {
                target_.fps = std::min(limits_.maxFps, target_.fps * 2);
            }
        }
        break;
    }
    return target_;
}

}  // namespace sd
