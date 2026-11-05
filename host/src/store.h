// Persisted state in %LOCALAPPDATA%\SideDroid. Pairing secrets are encrypted
// with DPAPI, so they are readable only by this Windows user.
#pragma once
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "sidedroid/secure.h"

namespace sd {

struct PairedDevice {
    ClientId id{};
    Key32 secret{};
    std::string name;
};

struct Settings {
    uint16_t width = 0;   // 0 = automatic
    uint16_t height = 0;
    uint16_t hz = 0;      // 0 = fastest the phone supports
    uint32_t maxKbps = 0; // 0 = automatic
    bool allowWireless = true;
    bool mirror = false;  // show the main display instead of an extended one
    uint16_t scalePercent = 0;  // display scaling of the virtual display, 0 = recommended
    uint8_t fill = 0;     // kFillFit and so on: the client's business, stored here so both sides agree
    bool consoleAutostartAsked = false;  // the console has asked about starting with Windows
    std::string encoder;  // "<kind>|<GPU name>", empty = automatic
    std::string vddGpu;   // which GPU renders the virtual displays, empty = Windows' choice
    // Where the virtual display last was, from before each device had its own. Only read now,
    // so that a device connecting for the first time after an upgrade finds its old place.
    int vddLeft = 0, vddTop = 0, vddRight = 0, vddBottom = 0;
};

// What one device shows. Until the user sets something for this device in particular
// (`custom`), the display fields follow the defaults in Settings. The place is always its own.
struct DeviceSettings {
    uint16_t width = 0, height = 0, hz = 0;
    uint32_t maxKbps = 0;
    bool mirror = false;
    uint16_t scalePercent = 0;
    bool custom = false;
    uint8_t fill = 0;
    int left = 0, top = 0, right = 0, bottom = 0;
    std::string encoder;
};

// The display fields of `s`, keeping everything else in `into`.
void copyDisplayDefaults(const Settings& s, DeviceSettings& into);
// Compact text form used in settings.ini. Pure, for testing.
std::string serializeDeviceSettings(const DeviceSettings& d);
bool parseDeviceSettings(const std::string& text, DeviceSettings& out);

class Store {
public:
    void load();
    std::optional<PairedDevice> findPaired(const ClientId& id);
    void addPaired(const PairedDevice& d);
    bool removePaired(const std::string& idHex);
    std::vector<PairedDevice> pairedList();

    Settings settings();
    void setSettings(const Settings& s);

    // A device seen for the first time starts from the defaults, and from where the
    // virtual display last was before displays were kept per device.
    DeviceSettings deviceSettings(const ClientId& id);
    void setDeviceSettings(const ClientId& id, const DeviceSettings& d);
    // New defaults apply to every device: choices made for one device end here. Places stay.
    void clearCustomDeviceSettings();

private:
    void savePaired();
    void saveSettings();
    std::mutex mutex_;
    std::vector<PairedDevice> paired_;
    Settings settings_;
    std::map<std::string, DeviceSettings> devices_;  // by client id in hex
};

std::string toHex(const uint8_t* p, size_t n);
bool fromHex(const std::string& s, uint8_t* out, size_t n);

}  // namespace sd
