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
//
// With too many modes the driver starts without any monitor at all. Measured with 4 monitors
// and 6 refresh rates: 14 sizes work and 15 do not. Whether sizes or sizes times rates is what
// counts was not settled, so both stay at what was seen to work. Past that, the oldest sizes
// go, except those in `modes` and `keep`, the ones devices are using.
std::optional<std::string> addModesToVddXml(const std::string& xml, const std::vector<DisplayMode>& modes,
                                            const std::vector<DisplayMode>& keep = {});

// Sets <monitors><count>. Pure, for testing. Returns the new document, or nullopt if nothing changed.
std::optional<std::string> setVddMonitorCountInXml(const std::string& xml, int count);

// Sets which GPU renders the virtual displays, "default" for Windows' choice. All of them
// share it: the driver has one setting. Pure, for testing. nullopt if nothing changed.
std::optional<std::string> setVddGpuInXml(const std::string& xml, const std::string& gpu);

// Default config path, or the one set in the driver's registry key.
std::wstring vddConfigPath();
// Adds any missing modes to the XML, sets the number of monitors unless it is 0, and the GPU
// unless it is empty. Returns false if the file cannot be written.
bool ensureVddConfig(const std::vector<DisplayMode>& modes, const std::vector<DisplayMode>& keep, int monitorCount, const std::string& gpu,
                     bool* changed);
// Asks the service to "enable", "disable" or "restart" the driver's device. Restarting is what
// makes the driver read its configuration. Returns the error, or nullopt if it is done.
// ERROR_FILE_NOT_FOUND means the service is not running.
std::optional<DWORD> askDisplayService(const char* word);
bool vddInstalled();

}  // namespace sd
