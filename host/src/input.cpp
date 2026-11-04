#include "input.h"

#include <algorithm>

#include "log.h"

namespace sd {

POINT mapTouchToDesktop(uint16_t x, uint16_t y, const RECT& d) {
    const LONG w = d.right - d.left, h = d.bottom - d.top;
    POINT p;
    p.x = d.left + static_cast<LONG>(static_cast<int64_t>(x) * (w > 0 ? w - 1 : 0) / 65535);
    p.y = d.top + static_cast<LONG>(static_cast<int64_t>(y) * (h > 0 ? h - 1 : 0) / 65535);
    return p;
}

TouchInjector::TouchInjector() {
    device_ = CreateSyntheticPointerDevice(PT_TOUCH, kMaxContacts, POINTER_FEEDBACK_DEFAULT);
    if (!device_) logf("CreateSyntheticPointerDevice failed: %lu", GetLastError());
}

TouchInjector::~TouchInjector() {
    releaseAll();
    if (device_) DestroySyntheticPointerDevice(device_);
}

void TouchInjector::setDisplayRect(const RECT& r) { rect_ = r; }

static POINTER_TYPE_INFO makeInfo(uint32_t pointerId, POINT pos, uint32_t pressure, POINTER_FLAGS flags) {
    POINTER_TYPE_INFO info{};
    info.type = PT_TOUCH;
    auto& t = info.touchInfo;
    t.pointerInfo.pointerType = PT_TOUCH;
    t.pointerInfo.pointerId = pointerId;
    t.pointerInfo.ptPixelLocation = pos;
    t.pointerInfo.pointerFlags = flags;
    t.touchFlags = TOUCH_FLAG_NONE;
    t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
    t.pressure = std::clamp<uint32_t>(pressure, 1, 1024);
    t.rcContact = {pos.x - 2, pos.y - 2, pos.x + 2, pos.y + 2};
    return info;
}

void TouchInjector::inject(const std::vector<TouchPoint>& points) {
    if (!device_ || rect_.right == rect_.left) return;

    // One frame must describe every contact that is down, in one call.
    const POINTER_FLAGS downFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    const POINTER_FLAGS moveFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;

    std::map<uint8_t, POINTER_TYPE_INFO> frame;
    std::vector<uint8_t> finished;
    for (const auto& p : points) {
        POINT pos = mapTouchToDesktop(p.x, p.y, rect_);
        auto it = active_.find(p.id);
        switch (p.action) {
        case TouchAction::Down:
            if (it != active_.end() || active_.size() >= kMaxContacts) break;
            it = active_.emplace(p.id, Contact{nextPointerId_, pos, p.pressure}).first;
            nextPointerId_ = nextPointerId_ % 200 + 1;
            frame[p.id] = makeInfo(it->second.pointerId, pos, p.pressure, downFlags);
            break;
        case TouchAction::Move:
            if (it == active_.end()) break;
            it->second.pos = pos;
            it->second.pressure = p.pressure;
            frame[p.id] = makeInfo(it->second.pointerId, pos, p.pressure, moveFlags);
            break;
        case TouchAction::Up:
            if (it == active_.end()) break;
            frame[p.id] = makeInfo(it->second.pointerId, pos, p.pressure, POINTER_FLAG_UP);
            finished.push_back(p.id);
            break;
        case TouchAction::Cancel:
            if (it == active_.end()) break;
            frame[p.id] = makeInfo(it->second.pointerId, it->second.pos, it->second.pressure,
                                   POINTER_FLAG_UP | POINTER_FLAG_CANCELED);
            finished.push_back(p.id);
            break;
        }
    }
    if (frame.empty()) return;

    std::vector<POINTER_TYPE_INFO> batch;
    for (const auto& [id, c] : active_) {
        auto f = frame.find(id);
        batch.push_back(f != frame.end() ? f->second : makeInfo(c.pointerId, c.pos, c.pressure, moveFlags));
    }
    send(batch);
    for (auto id : finished) active_.erase(id);
}

void TouchInjector::releaseAll() {
    if (!device_ || active_.empty()) return;
    std::vector<POINTER_TYPE_INFO> batch;
    for (const auto& [id, c] : active_)
        batch.push_back(makeInfo(c.pointerId, c.pos, c.pressure, POINTER_FLAG_UP | POINTER_FLAG_CANCELED));
    send(batch);
    active_.clear();
}

bool TouchInjector::send(const std::vector<POINTER_TYPE_INFO>& batch) {
    if (InjectSyntheticPointerInput(device_, batch.data(), static_cast<UINT32>(batch.size()))) return true;
    static int logged = 0;
    // ERROR_ACCESS_DENIED here usually means an elevated window has the foreground.
    if (logged++ < 5) logf("InjectSyntheticPointerInput failed: %lu", GetLastError());
    return false;
}

}  // namespace sd
