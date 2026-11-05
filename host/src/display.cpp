#include "display.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

#include "log.h"
#include "topology.h"

namespace sd {

bool isVddMonitorId(const std::wstring& id) {
    std::wstring up = id;
    std::transform(up.begin(), up.end(), up.begin(), ::towupper);
    return up.find(L"MTT1337") != std::wstring::npos;
}

std::vector<OutputInfo> listOutputs() {
    std::vector<OutputInfo> out;
    DISPLAY_DEVICEW ad{};
    ad.cb = sizeof(ad);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &ad, 0); ++i) {
        OutputInfo o;
        o.deviceName = ad.DeviceName;
        o.attached = (ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        o.primary = (ad.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

        DISPLAY_DEVICEW mon{};
        mon.cb = sizeof(mon);
        bool anyMonitor = false, hasVdd = false, hasOther = false;
        for (DWORD m = 0; EnumDisplayDevicesW(ad.DeviceName, m, &mon, EDD_GET_DEVICE_INTERFACE_NAME); ++m) {
            anyMonitor = true;
            if (isVddMonitorId(mon.DeviceID)) hasVdd = true;
            else hasOther = true;
            if (o.monitorId.empty()) o.monitorId = mon.DeviceID;
        }
        if (!anyMonitor) continue;  // adapter output with nothing behind it
        // In a clone group one output carries several monitors. Changing its mode
        // would change the real display's resolution as well.
        o.isVdd = hasVdd && !hasOther;
        o.isVddShared = hasVdd && hasOther;

        if (o.attached) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsExW(ad.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
                o.rect.left = dm.dmPosition.x;
                o.rect.top = dm.dmPosition.y;
                o.rect.right = dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth);
                o.rect.bottom = dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight);
            }
        }
        out.push_back(std::move(o));
    }
    return out;
}

std::optional<OutputInfo> findVddOutput() {
    for (auto& o : listOutputs())
        if (o.isVdd) return o;
    return std::nullopt;
}

std::optional<OutputInfo> findOutputByName(const std::wstring& name) {
    for (auto& o : listOutputs())
        if (o.deviceName == name) return o;
    return std::nullopt;
}

std::vector<DisplayMode> listModes(const std::wstring& deviceName) {
    std::vector<DisplayMode> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsExW(deviceName.c_str(), i, &dm, 0); ++i) {
        DisplayMode m{static_cast<uint16_t>(dm.dmPelsWidth), static_cast<uint16_t>(dm.dmPelsHeight),
                      static_cast<uint16_t>(dm.dmDisplayFrequency)};
        if (std::find(modes.begin(), modes.end(), m) == modes.end()) modes.push_back(m);
    }
    return modes;
}

bool outputHasMode(const std::wstring& deviceName, const DisplayMode& m) {
    auto modes = listModes(deviceName);
    return std::find(modes.begin(), modes.end(), m) != modes.end();
}

bool outputRect(const std::wstring& deviceName, RECT* out) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) return false;
    out->left = dm.dmPosition.x;
    out->top = dm.dmPosition.y;
    out->right = dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth);
    out->bottom = dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight);
    return true;
}

POINT placeDisplay(const RECT* previous, LONG width, LONG height, const std::vector<RECT>& others) {
    const auto spanOverlap = [](LONG a1, LONG a2, LONG b1, LONG b2) { return a1 < b2 && b1 < a2; };
    const auto at = [&](LONG x, LONG y) { return RECT{x, y, x + width, y + height}; };
    // Windows only accepts a desktop with no overlaps and nothing left floating.
    const auto fits = [&](const RECT& r) {
        if (others.empty()) return true;
        bool touching = false;
        for (const auto& o : others) {
            const bool overX = spanOverlap(r.left, r.right, o.left, o.right);
            const bool overY = spanOverlap(r.top, r.bottom, o.top, o.bottom);
            if (overX && overY) return false;
            if ((overY && (r.right == o.left || r.left == o.right)) || (overX && (r.bottom == o.top || r.top == o.bottom)))
                touching = true;
        }
        return touching;
    };

    if (previous) {
        const RECT& p = *previous;
        LONG x = p.left, y = p.top;
        const RECT* beside = nullptr;  // a display it touches on its left or right
        const RECT* stacked = nullptr;  // a display it touches above or below
        for (const auto& o : others) {
            const bool overX = spanOverlap(p.left, p.right, o.left, o.right);
            const bool overY = spanOverlap(p.top, p.bottom, o.top, o.bottom);
            if (overY && p.right == o.left) x = o.left - width;
            if (overX && p.bottom == o.top) y = o.top - height;
            if (overY && (p.right == o.left || p.left == o.right)) beside = &o;
            if (overX && (p.bottom == o.top || p.top == o.bottom)) stacked = &o;
        }
        if (fits(at(x, y))) return {x, y};
        // Turning the phone can slide it past the end of the display it was next to.
        if (beside && fits(at(x, beside->top))) return {x, beside->top};
        if (stacked && fits(at(stacked->left, y))) return {stacked->left, y};
    }

    // Right of everything else, top aligned.
    LONG x = 0;
    for (const auto& o : others) x = std::max(x, o.right);
    return {x, 0};
}

bool attachOutput(const std::wstring& deviceName, const DisplayMode& m, const RECT* previous) {
    auto self = findOutputByName(deviceName);
    if (self && !self->isVdd) {
        logf("refusing to change the mode of %s: it is not only the virtual display", narrow(deviceName).c_str());
        return false;
    }
    const bool attached = self && self->attached;

    std::vector<RECT> others;
    for (const auto& o : listOutputs())
        if (o.attached && o.deviceName != deviceName) others.push_back(o.rect);
    const RECT* from = previous ? previous : attached ? &self->rect : nullptr;
    const POINT pos = placeDisplay(from, m.width, m.height, others);
    // The main display defines the origin of the desktop, so it never moves.
    const bool keepPosition = attached && self->primary;

    // A pipeline rebuild for bitrate reasons asks for what it already has.
    // Setting it again would make the whole desktop flicker.
    if (attached) {
        DEVMODEW cur{};
        cur.dmSize = sizeof(cur);
        if (EnumDisplaySettingsExW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &cur, 0) && cur.dmPelsWidth == m.width &&
            cur.dmPelsHeight == m.height && cur.dmDisplayFrequency == m.hz &&
            (keepPosition || (cur.dmPosition.x == pos.x && cur.dmPosition.y == pos.y))) {
            return true;
        }
    }

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    dm.dmPelsWidth = m.width;
    dm.dmPelsHeight = m.height;
    dm.dmDisplayFrequency = m.hz;
    if (!keepPosition) {
        dm.dmFields |= DM_POSITION;
        dm.dmPosition.x = pos.x;
        dm.dmPosition.y = pos.y;
    }
    LONG r = ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (r == DISP_CHANGE_SUCCESSFUL) r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        logf("attachOutput %s %dx%d@%d failed: %ld", narrow(deviceName).c_str(), m.width, m.height, m.hz, r);
        return false;
    }
    return true;
}

const std::vector<uint16_t>& scaleSteps() {
    static const std::vector<uint16_t> steps{100, 125, 150, 175, 200, 225, 250, 300, 350};
    return steps;
}

uint16_t recommendedScale(uint16_t width, uint16_t height, uint16_t densityDpi) {
    const int longer = std::max(width, height), shorter = std::min(width, height);
    double limit = std::min(longer / 1280.0, shorter / 600.0);
    if (densityDpi > 0) limit = std::min(limit, densityDpi / 160.0);
    uint16_t best = 100;
    for (uint16_t s : scaleSteps())
        if (s <= limit * 100.0 + 0.01) best = s;
    return best;
}

std::vector<Size> commonResolutions(uint16_t maxLong, uint16_t maxShort) {
    static const Size all[] = {{3840, 2160}, {3440, 1440}, {2880, 1800}, {2560, 1600}, {2560, 1440}, {2400, 1080},
                               {1920, 1200}, {1920, 1080}, {1680, 1050}, {1600, 900},  {1366, 768},  {1280, 800},
                               {1280, 720}};
    std::vector<Size> out;
    for (const auto& s : all)
        if ((maxLong == 0 || s.width <= maxLong) && (maxShort == 0 || s.height <= maxShort)) out.push_back(s);
    return out;
}

Size primaryDisplaySize() {
    for (const auto& o : listOutputs()) {
        if (o.primary)
            return {static_cast<uint16_t>(o.rect.right - o.rect.left), static_cast<uint16_t>(o.rect.bottom - o.rect.top)};
    }
    return {};
}

DisplayMode chooseMode(const Hello& hello, uint16_t vpW, uint16_t vpH, const SetDisplay& manualIn, uint16_t pcW, uint16_t pcH) {
    SetDisplay manual = manualIn;
    if (manual.width == kResolutionMatchPc) {
        manual.width = pcW;
        manual.height = pcH;
    }
    uint16_t w = vpW ? vpW : hello.nativeWidth;
    uint16_t h = vpH ? vpH : hello.nativeHeight;
    if (w == 0 || h == 0) { w = 1920; h = 1080; }
    const bool portrait = h > w;

    if (manual.width && manual.height) {
        uint16_t longer = std::max(manual.width, manual.height);
        uint16_t shorter = std::min(manual.width, manual.height);
        w = portrait ? shorter : longer;
        h = portrait ? longer : shorter;
    }

    // Never ask for more than the phone's decoder can take, keeping aspect.
    uint16_t maxLong = std::max(hello.maxDecodeWidth, hello.maxDecodeHeight);
    uint16_t maxShort = std::min(hello.maxDecodeWidth, hello.maxDecodeHeight);
    if (maxLong && maxShort) {
        double scale = std::min({1.0, static_cast<double>(maxLong) / std::max(w, h),
                                 static_cast<double>(maxShort) / std::min(w, h)});
        w = static_cast<uint16_t>(w * scale);
        h = static_cast<uint16_t>(h * scale);
    }
    // Encoders need even dimensions.
    w &= ~1;
    h &= ~1;

    uint16_t hz = 60;
    if (!hello.refreshRates.empty()) hz = *std::max_element(hello.refreshRates.begin(), hello.refreshRates.end());
    if (manual.hz) {
        // A rate the phone cannot show would only waste encoder and network.
        hz = manual.hz;
        if (!hello.refreshRates.empty() &&
            std::find(hello.refreshRates.begin(), hello.refreshRates.end(), manual.hz) == hello.refreshRates.end()) {
            uint16_t best = 0;
            for (auto r : hello.refreshRates)
                if (r <= manual.hz && r > best) best = r;
            hz = best ? best : *std::min_element(hello.refreshRates.begin(), hello.refreshRates.end());
        }
    }
    return {w, h, hz};
}

static std::optional<int> numberInTag(const std::string& xml, const char* tag, size_t from, size_t to) {
    std::string open = std::string("<") + tag + ">";
    auto a = xml.find(open, from);
    if (a == std::string::npos || a >= to) return std::nullopt;
    return atoi(xml.c_str() + a + open.size());
}

std::optional<std::string> addModesToVddXml(const std::string& xmlIn, const std::vector<DisplayMode>& modes) {
    std::string xml = xmlIn;
    bool changed = false;

    // Refresh rates are global in this driver: each one applies to every resolution.
    std::set<int> rates;
    {
        const std::string tag = "<g_refresh_rate>";
        for (size_t p = xml.find(tag); p != std::string::npos; p = xml.find(tag, p + 1))
            rates.insert(atoi(xml.c_str() + p + tag.size()));
    }
    auto globalEnd = xml.find("</global>");
    if (globalEnd == std::string::npos) return std::nullopt;
    std::string addRates;
    for (const auto& m : modes) {
        if (rates.insert(m.hz).second) {
            addRates += "\t\t<g_refresh_rate>" + std::to_string(m.hz) + "</g_refresh_rate>\n\t";
            changed = true;
        }
    }
    xml.insert(globalEnd, addRates);

    // Existing width and height pairs.
    std::set<std::pair<int, int>> sizes;
    {
        const std::string tag = "<resolution>", endTag = "</resolution>";
        for (size_t p = xml.find(tag); p != std::string::npos; p = xml.find(tag, p + 1)) {
            size_t e = xml.find(endTag, p);
            if (e == std::string::npos) break;
            auto w = numberInTag(xml, "width", p, e);
            auto h = numberInTag(xml, "height", p, e);
            if (w && h) sizes.insert({*w, *h});
        }
    }
    auto resEnd = xml.find("</resolutions>");
    if (resEnd == std::string::npos) return std::nullopt;
    std::string addSizes;
    for (const auto& m : modes) {
        if (sizes.insert({m.width, m.height}).second) {
            std::ostringstream os;
            os << "    <resolution>\n            <width>" << m.width << "</width>\n            <height>" << m.height
               << "</height>\n            <refresh_rate>" << m.hz << "</refresh_rate>\n        </resolution>\n    ";
            addSizes += os.str();
            changed = true;
        }
    }
    xml.insert(resEnd, addSizes);

    if (!changed) return std::nullopt;
    return xml;
}

std::wstring vddConfigPath() {
    wchar_t buf[MAX_PATH]{};
    DWORD size = sizeof(buf);
    std::wstring dir = L"C:\\VirtualDisplayDriver";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", L"VDDPATH", RRF_RT_REG_SZ,
                     nullptr, buf, &size) == ERROR_SUCCESS && buf[0]) {
        dir = buf;
    }
    return dir + L"\\vdd_settings.xml";
}

bool ensureVddModes(const std::vector<DisplayMode>& modes, bool* changed) {
    if (changed) *changed = false;
    auto path = vddConfigPath();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        logf("cannot read %s", narrow(path).c_str());
        return false;
    }
    std::string xml;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) xml.append(buf, n);
    fclose(f);

    auto updated = addModesToVddXml(xml, modes);
    if (!updated) return true;

    // Write beside it and swap, so the driver never reads half a file.
    auto tmp = path + L".tmp";
    f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        logf("cannot write %s", narrow(tmp).c_str());
        return false;
    }
    fwrite(updated->data(), 1, updated->size(), f);
    fclose(f);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logf("cannot replace %s: %lu", narrow(path).c_str(), GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (changed) *changed = true;
    return true;
}

bool vddCommand(const std::wstring& command) {
    constexpr auto name = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";
    // The driver serves one client at a time and reports busy in between.
    for (int attempt = 0; attempt < 10; ++attempt) {
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            BOOL ok = WriteFile(h, command.data(), static_cast<DWORD>(command.size() * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(h);
            return ok != FALSE;
        }
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return false;
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(name, 300);
        else Sleep(100);
    }
    return false;
}

bool vddInstalled() { return vddMonitorPresent(); }

}  // namespace sd
