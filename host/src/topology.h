// Display layout through the Windows CCD API: saving it, putting the virtual
// display on its own source, and putting everything back.
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sd {

struct DisplayLayout {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

// Pure helpers, for testing.
//
// Drops the paths `remove` selects, then drops modes nothing refers to any more
// and renumbers the remaining references. SetDisplayConfig rejects dangling ones.
DisplayLayout layoutWithout(const DisplayLayout& in, const std::function<bool(const DISPLAYCONFIG_PATH_INFO&)>& remove);
std::string serializeLayout(const DisplayLayout& layout);
bool deserializeLayout(const std::string& data, DisplayLayout& out);

// Windows side.
bool queryActiveLayout(DisplayLayout& out);
bool applyLayout(const DisplayLayout& layout, bool saveToDatabase);

bool vddMonitorPresent();
bool isVddPath(const DISPLAYCONFIG_PATH_INFO& path);
// Active layout minus the virtual display: what the desktop looks like without it.
bool queryLayoutWithoutVdd(DisplayLayout& out);

// The driver can show several monitors, one for each connected device. A monitor is
// known by its device path, which outlives a driver reload, unlike its GDI name.
std::vector<std::wstring> listVddMonitors();
// GDI name of the source showing this monitor, or empty when it is not on the desktop.
std::wstring vddMonitorSource(const std::wstring& monitor);
// True if this monitor is part of the layout.
bool layoutHasMonitor(const DisplayLayout& layout, const std::wstring& monitor);
// Takes one monitor off the desktop and leaves everything else as it is.
bool removeVddMonitor(const std::wstring& monitor);

// Makes this monitor an extended desktop of its own, leaving real displays and the
// monitors in `keep` as they are. Other monitors of the driver leave the desktop.
// Windows may have put it into a clone group, which is what makes a phone mirror the
// laptop. Returns the GDI name of its source, or empty on failure.
std::wstring isolateVdd(const std::wstring& monitor, const std::vector<std::wstring>& keep);

// Display scaling of this monitor, the same setting as in Windows' display settings.
// Windows has no public API for this one, and it only stores steps relative to its
// own recommendation. Returns 0 when unavailable.
uint16_t getVddScale(const std::wstring& monitor);
bool setVddScale(const std::wstring& monitor, uint16_t percent);

}  // namespace sd
