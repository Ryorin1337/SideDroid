// Tracks changes in frame arrival delay without synchronizing the two clocks.
#pragma once
#include <cstdint>
#include <mutex>

namespace sd {

class DelayStats {
public:
    struct Window {
        uint64_t samples = 0;
        int32_t gradientUs = 0;
    };

    void add(uint64_t captureUs, uint64_t arrivalUs);
    Window take();

private:
    std::mutex mutex_;
    bool haveOrigin_ = false, havePrevious_ = false;
    uint64_t captureOriginUs_ = 0, arrivalOriginUs_ = 0;
    uint64_t samples_ = 0;
    long double sumUs_ = 0, previousAverageUs_ = 0;
};

}  // namespace sd
