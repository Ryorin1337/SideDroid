#include "topology.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "display.h"
#include "log.h"

namespace sd {

namespace {

constexpr uint32_t kLayoutMagic = 0x3144534C;  // "LSD1"

bool sameAdapter(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

bool queryLayout(UINT32 flags, DisplayLayout& out) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        UINT32 np = 0, nm = 0;
        LONG r = GetDisplayConfigBufferSizes(flags, &np, &nm);
        if (r != ERROR_SUCCESS) return false;
        out.paths.resize(np);
        out.modes.resize(nm);
        r = QueryDisplayConfig(flags, &np, out.paths.data(), &nm, out.modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;  // layout changed between the two calls
        if (r != ERROR_SUCCESS) {
            logf("QueryDisplayConfig failed: %ld", r);
            return false;
        }
        out.paths.resize(np);
        out.modes.resize(nm);
        return true;
    }
    return false;
}

std::wstring targetPathOf(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME n{};
    n.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    n.header.size = sizeof(n);
    n.header.adapterId = path.targetInfo.adapterId;
    n.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&n.header) != ERROR_SUCCESS) return {};
    return n.monitorDevicePath;
}

std::wstring sourceNameOf(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME n{};
    n.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    n.header.size = sizeof(n);
    n.header.adapterId = path.sourceInfo.adapterId;
    n.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&n.header) != ERROR_SUCCESS) return {};
    return n.viewGdiDeviceName;
}

}  // namespace

DisplayLayout layoutWithout(const DisplayLayout& in, const std::function<bool(const DISPLAYCONFIG_PATH_INFO&)>& remove) {
    DisplayLayout out;
    std::map<UINT32, UINT32> renumber;
    auto keepMode = [&](UINT32 index) -> UINT32 {
        if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || index >= in.modes.size()) return DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        auto it = renumber.find(index);
        if (it != renumber.end()) return it->second;
        const auto next = static_cast<UINT32>(out.modes.size());
        out.modes.push_back(in.modes[index]);
        renumber[index] = next;
        return next;
    };
    for (const auto& p : in.paths) {
        if (remove(p)) continue;
        DISPLAYCONFIG_PATH_INFO copy = p;
        copy.sourceInfo.modeInfoIdx = keepMode(p.sourceInfo.modeInfoIdx);
        copy.targetInfo.modeInfoIdx = keepMode(p.targetInfo.modeInfoIdx);
        out.paths.push_back(copy);
    }
    return out;
}

std::string serializeLayout(const DisplayLayout& l) {
    std::string s;
    const uint32_t head[3] = {kLayoutMagic, static_cast<uint32_t>(l.paths.size()), static_cast<uint32_t>(l.modes.size())};
    s.append(reinterpret_cast<const char*>(head), sizeof(head));
    if (!l.paths.empty()) s.append(reinterpret_cast<const char*>(l.paths.data()), l.paths.size() * sizeof(l.paths[0]));
    if (!l.modes.empty()) s.append(reinterpret_cast<const char*>(l.modes.data()), l.modes.size() * sizeof(l.modes[0]));
    return s;
}

bool deserializeLayout(const std::string& data, DisplayLayout& out) {
    uint32_t head[3];
    if (data.size() < sizeof(head)) return false;
    std::memcpy(head, data.data(), sizeof(head));
    if (head[0] != kLayoutMagic || head[1] > 256 || head[2] > 1024) return false;
    const size_t pathBytes = size_t{head[1]} * sizeof(DISPLAYCONFIG_PATH_INFO);
    const size_t modeBytes = size_t{head[2]} * sizeof(DISPLAYCONFIG_MODE_INFO);
    if (data.size() != sizeof(head) + pathBytes + modeBytes) return false;
    out.paths.resize(head[1]);
    out.modes.resize(head[2]);
    if (pathBytes) std::memcpy(out.paths.data(), data.data() + sizeof(head), pathBytes);
    if (modeBytes) std::memcpy(out.modes.data(), data.data() + sizeof(head) + pathBytes, modeBytes);
    return true;
}

bool queryActiveLayout(DisplayLayout& out) { return queryLayout(QDC_ONLY_ACTIVE_PATHS, out); }

bool applyLayout(const DisplayLayout& l, bool save) {
    if (l.paths.empty()) return false;  // an empty layout would turn off every display
    UINT32 flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
    if (save) flags |= SDC_SAVE_TO_DATABASE;
    auto copy = l;
    LONG r = SetDisplayConfig(static_cast<UINT32>(copy.paths.size()), copy.paths.data(),
                              static_cast<UINT32>(copy.modes.size()), copy.modes.data(), flags);
    if (r != ERROR_SUCCESS) {
        logf("SetDisplayConfig failed: %ld", r);
        return false;
    }
    return true;
}

bool isVddPath(const DISPLAYCONFIG_PATH_INFO& path) { return isVddMonitorId(targetPathOf(path)); }

bool vddMonitorPresent() {
    DisplayLayout all;
    if (!queryLayout(QDC_ALL_PATHS, all)) return false;
    return std::any_of(all.paths.begin(), all.paths.end(), [](const auto& p) {
        return p.targetInfo.targetAvailable && isVddPath(p);
    });
}

bool vddOnDesktop() {
    DisplayLayout active;
    if (!queryActiveLayout(active)) return false;
    return std::any_of(active.paths.begin(), active.paths.end(), [](const auto& p) { return isVddPath(p); });
}

bool queryLayoutWithoutVdd(DisplayLayout& out) {
    DisplayLayout active;
    if (!queryActiveLayout(active)) return false;
    out = layoutWithout(active, [](const auto& p) { return isVddPath(p); });
    return true;
}

std::wstring isolateVdd() {
    DisplayLayout active;
    if (!queryActiveLayout(active)) return {};

    // Already on the desktop with a source to itself: nothing to change.
    for (const auto& p : active.paths) {
        if (!isVddPath(p)) continue;
        const auto sharing = std::count_if(active.paths.begin(), active.paths.end(), [&](const auto& o) {
            return sameAdapter(o.sourceInfo.adapterId, p.sourceInfo.adapterId) && o.sourceInfo.id == p.sourceInfo.id;
        });
        if (sharing == 1) return sourceNameOf(p);
    }

    DisplayLayout others = layoutWithout(active, [](const auto& p) { return isVddPath(p); });
    if (others.paths.empty()) {
        logf("no other display is active; refusing to change the layout");
        return {};
    }

    DisplayLayout all;
    if (!queryLayout(QDC_ALL_PATHS, all)) return {};

    // A source on the driver's adapter that no other display is using.
    const DISPLAYCONFIG_PATH_INFO* candidate = nullptr;
    for (const auto& p : all.paths) {
        if (!p.targetInfo.targetAvailable || !isVddPath(p)) continue;
        const bool used = std::any_of(others.paths.begin(), others.paths.end(), [&](const auto& o) {
            return sameAdapter(o.sourceInfo.adapterId, p.sourceInfo.adapterId) && o.sourceInfo.id == p.sourceInfo.id;
        });
        if (!used) {
            candidate = &p;
            break;
        }
    }
    if (!candidate) {
        logf("no free display source for the virtual display");
        return {};
    }

    DISPLAYCONFIG_PATH_INFO added = *candidate;
    added.flags = DISPLAYCONFIG_PATH_ACTIVE;
    added.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    added.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    DisplayLayout wanted = others;
    wanted.paths.push_back(added);
    if (!applyLayout(wanted, false)) return {};

    // The source a display ended up on is only certain once Windows reports it.
    DisplayLayout after;
    if (!queryActiveLayout(after)) return {};
    for (const auto& p : after.paths) {
        if (!isVddPath(p)) continue;
        const auto sharing = std::count_if(after.paths.begin(), after.paths.end(), [&](const auto& o) {
            return sameAdapter(o.sourceInfo.adapterId, p.sourceInfo.adapterId) && o.sourceInfo.id == p.sourceInfo.id;
        });
        if (sharing == 1) return sourceNameOf(p);
    }
    logf("virtual display is still sharing a source after the layout change");
    return {};
}

}  // namespace sd
