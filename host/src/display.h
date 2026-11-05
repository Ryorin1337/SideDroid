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

// Current place of an output on the desktop, cheap enough to poll.
bool outputRect(const std::wstring& deviceName, RECT* out);

// Where a display of the given size goes, given where it was and the displays around
// it. It stays on the side the user put it: a display left of another keeps touching
// it as it grows. When that no longer fits, it goes right of everything. Pure, for testing.
POINT placeDisplay(const RECT* previous, LONG width, LONG height, const std::vector<RECT>& others);

// Puts the output on the desktop in mode m, near `previous` if given, otherwise where
// it already is. Without either, right of the existing displays.
bool attachOutput(const std::wstring& deviceName, const DisplayMode& m, const RECT* previous = nullptr);

// Picks what to request from Windows for this device.
//   manual non-zero fields win; otherwise native size and the fastest rate.
// Orientation of manual sizes follows the device's current orientation.
// kResolutionMatchPc asks for the size of the PC's main display, given as pcW and pcH.
DisplayMode chooseMode(const Hello& hello, uint16_t viewportW, uint16_t viewportH, const SetDisplay& manual,
                       uint16_t pcW = 0, uint16_t pcH = 0);

// The steps Windows offers for display scaling, in percent.
const std::vector<uint16_t>& scaleSteps();
// The largest step that keeps a desktop usable on this panel: a logical size of at
// least 1280x600 for windows designed for a PC, and controls no larger than the
// phone's own apps would draw them. Pure, for testing.
uint16_t recommendedScale(uint16_t width, uint16_t height, uint16_t densityDpi);
// Common sizes this decoder can take, largest first. Zero limits mean unknown.
std::vector<Size> commonResolutions(uint16_t maxLong, uint16_t maxShort);
// Physical size of the main display.
Size primaryDisplaySize();

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
