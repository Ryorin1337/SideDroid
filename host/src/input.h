// Injects touch contacts from the phone as real Windows touch input.
#pragma once
#include <windows.h>

#include <map>
#include <vector>

#include "sidedroid/wire.h"

namespace sd {

// Normalized phone coordinates (0..65535) to desktop pixels. Pure, for testing.
POINT mapTouchToDesktop(uint16_t x, uint16_t y, const RECT& display);

class TouchInjector {
public:
    TouchInjector();
    ~TouchInjector();
    bool available() const { return device_ != nullptr; }
    void setDisplayRect(const RECT& r);
    void inject(const std::vector<TouchPoint>& points);
    // Lifts every finger still down, so that nothing stays stuck after a disconnect.
    void releaseAll();

private:
    struct Contact { uint32_t pointerId; POINT pos; uint32_t pressure; };
    bool send(const std::vector<POINTER_TYPE_INFO>& batch);

    HSYNTHETICPOINTERDEVICE device_ = nullptr;
    RECT rect_{};
    std::map<uint8_t, Contact> active_;
    uint32_t nextPointerId_ = 1;
    static constexpr int kMaxContacts = 10;
};

}  // namespace sd
