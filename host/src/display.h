// The Windows side of "a display": finding the Virtual Display Driver monitor,
// attaching it to the desktop, changing its mode, and listing outputs.
#pragma once
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "sidedroid/wire.h"

namespace sd {

struct OutputInfo {
    std::wstring deviceName;   // \\.\DISPLAY3
    std::wstring monitorId;    // device interface path of the monitor
    bool attached = false;     // part of the desktop
    bool primary = false;
    RECT rect{};
    bool isVdd = false;        // the virtual display and nothing else
    bool isVddShared = false;  // in a clone group with a real display: never change its mode
};

std::vector<OutputInfo> listOutputs();

// Only monitors whose hardware id contains MTT1337 count. Other virtual
// displays on the machine (VR streaming, remote desktop) are never touched.
bool isVddMonitorId(const std::wstring& deviceId);
std::optional<OutputInfo> findVddOutput();
std::optional<OutputInfo> findOutputByName(const std::wstring& deviceName);

std::vector<DisplayMode> listModes(const std::wstring& deviceName);
bool outputHasMode(const std::wstring& deviceName, const DisplayMode& m);

// Puts the output on the desktop, right of the existing displays, in mode m.
bool attachOutput(const std::wstring& deviceName, const DisplayMode& m);

// Picks what to request from Windows for this device.
//   manual non-zero fields win; otherwise native size and the fastest rate.
// Orientation of manual sizes follows the device's current orientation.
DisplayMode chooseMode(const Hello& hello, uint16_t viewportW, uint16_t viewportH, const SetDisplay& manual);

// Edits only <resolutions>, <g_refresh_rate> entries; every other setting stays.
// Pure, for testing. Returns the new document, or nullopt if nothing changed.
std::optional<std::string> addModesToVddXml(const std::string& xml, const std::vector<DisplayMode>& modes);

// Default config path, or the one set in the driver's registry key.
std::wstring vddConfigPath();
// Adds any missing modes to the XML. Returns false if the file cannot be written.
bool ensureVddModes(const std::vector<DisplayMode>& modes, bool* changed);
// Sends a command to \\.\pipe\MTTVirtualDisplayPipe. Returns false if the driver is not there.
bool vddCommand(const std::wstring& command);
bool vddInstalled();

}  // namespace sd
