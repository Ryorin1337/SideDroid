#include "topology.h"

#include <algorithm>
#include <cstring>
#include <iterator>
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

bool queryLayoutWithoutVdd(DisplayLayout& out) {
    DisplayLayout active;
    if (!queryActiveLayout(active)) return false;
    out = layoutWithout(active, [](const auto& p) { return isVddPath(p); });
    return true;
}

namespace {

// Undocumented, used through DisplayConfigGetDeviceInfo and DisplayConfigSetDeviceInfo.
constexpr int kGetDpiScale = -3;
constexpr int kSetDpiScale = -4;
struct DpiScaleGet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t minRelative;
    int32_t currentRelative;
    int32_t maxRelative;
};
struct DpiScaleSet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t relative;
};
// All the steps Windows counts through, whichever of them our menus offer.
constexpr uint16_t kWindowsScales[] = {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};

bool findVddSource(const std::wstring& monitor, DISPLAYCONFIG_PATH_SOURCE_INFO* out) {
    DisplayLayout active;
    if (!queryActiveLayout(active)) return false;
    for (const auto& p : active.paths) {
        if (targetPathOf(p) == monitor) {
            *out = p.sourceInfo;
            return true;
        }
    }
    return false;
}

bool queryDpi(const DISPLAYCONFIG_PATH_SOURCE_INFO& src, DpiScaleGet* g) {
    *g = {};
    g->header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetDpiScale);
    g->header.size = sizeof(*g);
    g->header.adapterId = src.adapterId;
    g->header.id = src.id;
    return DisplayConfigGetDeviceInfo(&g->header) == ERROR_SUCCESS;
}

}  // namespace

uint16_t getVddScale(const std::wstring& monitor) {
    DISPLAYCONFIG_PATH_SOURCE_INFO src{};
    DpiScaleGet g;
    if (!findVddSource(monitor, &src) || !queryDpi(src, &g)) return 0;
    const int index = -g.minRelative + g.currentRelative;
    if (index < 0 || index >= static_cast<int>(std::size(kWindowsScales))) return 0;
    return kWindowsScales[index];
}

bool setVddScale(const std::wstring& monitor, uint16_t percent) {
    DISPLAYCONFIG_PATH_SOURCE_INFO src{};
    DpiScaleGet g;
    if (!findVddSource(monitor, &src) || !queryDpi(src, &g)) return false;
    int target = -1;
    for (int i = 0; i < static_cast<int>(std::size(kWindowsScales)); ++i)
        if (kWindowsScales[i] == percent) target = i;
    if (target < 0) return false;
    // Relative to the step Windows recommends, which is step -minRelative. A panel
    // can limit how far it goes, so an out of range request takes the nearest one.
    const int relative = std::clamp(target + g.minRelative, g.minRelative, g.maxRelative);
    if (relative == g.currentRelative) return true;
    DpiScaleSet s{};
    s.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kSetDpiScale);
    s.header.size = sizeof(s);
    s.header.adapterId = src.adapterId;
    s.header.id = src.id;
    s.relative = relative;
    const LONG r = DisplayConfigSetDeviceInfo(&s.header);
    if (r != ERROR_SUCCESS) {
        logf("setting display scale failed: %ld", r);
        return false;
    }
    return true;
}

std::vector<std::wstring> listVddMonitors() {
    std::set<std::wstring> found;
    DisplayLayout all;
    if (queryLayout(QDC_ALL_PATHS, all)) {
        for (const auto& p : all.paths) {
            if (!p.targetInfo.targetAvailable) continue;
            auto path = targetPathOf(p);
            if (isVddMonitorId(path)) found.insert(std::move(path));
        }
    }
    return {found.begin(), found.end()};
}

std::wstring vddMonitorSource(const std::wstring& monitor) {
    DisplayLayout active;
    if (!queryActiveLayout(active)) return {};
    for (const auto& p : active.paths)
        if (targetPathOf(p) == monitor) return sourceNameOf(p);
    return {};
}

bool layoutHasMonitor(const DisplayLayout& layout, const std::wstring& monitor) {
    return std::any_of(layout.paths.begin(), layout.paths.end(), [&](const auto& p) { return targetPathOf(p) == monitor; });
}

bool removeVddMonitor(const std::wstring& monitor) {
    DisplayLayout active;
    if (!queryActiveLayout(active)) return false;
    if (!layoutHasMonitor(active, monitor)) return true;
    return applyLayout(layoutWithout(active, [&](const auto& p) { return targetPathOf(p) == monitor; }), false);
}

std::wstring isolateVdd(const std::wstring& monitor, const std::vector<std::wstring>& keep) {
    DisplayLayout active;
    if (!queryActiveLayout(active)) return {};

    const auto stray = [&](const DISPLAYCONFIG_PATH_INFO& p) {
        const auto path = targetPathOf(p);
        return isVddMonitorId(path) && path != monitor && std::find(keep.begin(), keep.end(), path) == keep.end();
    };
    const auto ownSource = [](const DisplayLayout& l, const DISPLAYCONFIG_PATH_INFO& p) {
        return std::count_if(l.paths.begin(), l.paths.end(), [&](const auto& o) {
            return sameAdapter(o.sourceInfo.adapterId, p.sourceInfo.adapterId) && o.sourceInfo.id == p.sourceInfo.id;
        }) == 1;
    };

    // Already on the desktop with a source to itself, and no monitor there without
    // a device behind it: nothing to change.
    if (std::none_of(active.paths.begin(), active.paths.end(), stray)) {
        for (const auto& p : active.paths)
            if (targetPathOf(p) == monitor && ownSource(active, p)) return sourceNameOf(p);
    }

    // After a driver reload Windows may clone the driver's monitors onto a real display. While
    // it does, it reports them on that display's adapter, with that source as the only choice,
    // so they have to leave the clone group before any of them can get a source of its own.
    // One that belongs to another device is put back by that device's session.
    const auto cloned = [&](const DISPLAYCONFIG_PATH_INFO& p) { return isVddPath(p) && !ownSource(active, p); };
    if (std::any_of(active.paths.begin(), active.paths.end(), cloned)) {
        logf("taking virtual displays out of a clone group");
        if (!applyLayout(layoutWithout(active, cloned), false)) return {};
        Sleep(500);
        if (!queryActiveLayout(active)) return {};
    }

    DisplayLayout others = layoutWithout(active, [&](const auto& p) { return targetPathOf(p) == monitor || stray(p); });
    if (std::none_of(others.paths.begin(), others.paths.end(), [](const auto& p) { return !isVddPath(p); })) {
        logf("no other display is active; refusing to change the layout");
        return {};
    }

    // A source on the driver's adapter that no other display is using. Paths can take a
    // moment to settle after the layout changed.
    DisplayLayout all;
    const DISPLAYCONFIG_PATH_INFO* candidate = nullptr;
    for (int attempt = 0; attempt < 10 && !candidate; ++attempt) {
        if (attempt > 0) Sleep(200);
        if (!queryLayout(QDC_ALL_PATHS, all)) return {};
        for (const auto& p : all.paths) {
            if (!p.targetInfo.targetAvailable || targetPathOf(p) != monitor) continue;
            const bool used = std::any_of(others.paths.begin(), others.paths.end(), [&](const auto& o) {
                return sameAdapter(o.sourceInfo.adapterId, p.sourceInfo.adapterId) && o.sourceInfo.id == p.sourceInfo.id;
            });
            if (!used) {
                candidate = &p;
                break;
            }
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
    for (const auto& p : after.paths)
        if (targetPathOf(p) == monitor && ownSource(after, p)) return sourceNameOf(p);
    logf("virtual display is still sharing a source after the layout change");
    return {};
}

}  // namespace sd
