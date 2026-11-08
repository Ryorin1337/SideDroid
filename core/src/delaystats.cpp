#include "sidedroid/delaystats.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sd {

namespace {

long double elapsedUs(uint64_t now, uint64_t origin) {
    return now >= origin ? static_cast<long double>(now - origin)
                         : -static_cast<long double>(origin - now);
}

}  // namespace

void DelayStats::add(uint64_t captureUs, uint64_t arrivalUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!haveOrigin_) {
        captureOriginUs_ = captureUs;
        arrivalOriginUs_ = arrivalUs;
        haveOrigin_ = true;
    }
    // Subtract each clock's origin before combining them, so a large fixed
    // clock offset never enters the window sum or amplifies rounding errors.
    sumUs_ += elapsedUs(arrivalUs, arrivalOriginUs_) - elapsedUs(captureUs, captureOriginUs_);
    ++samples_;
}

DelayStats::Window DelayStats::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    Window result;
    result.samples = samples_;
    if (samples_ == 0) return result;

    const long double average = std::trunc(sumUs_ / samples_);
    if (havePrevious_) {
        const long double gradient = average - previousAverageUs_;
        result.gradientUs = static_cast<int32_t>(std::clamp(gradient,
            static_cast<long double>(std::numeric_limits<int32_t>::min()),
            static_cast<long double>(std::numeric_limits<int32_t>::max())));
    }
    previousAverageUs_ = average;
    havePrevious_ = true;
    sumUs_ = 0;
    samples_ = 0;
    return result;
}

}  // namespace sd
