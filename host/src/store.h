// Persisted state in %LOCALAPPDATA%\SideDroid. Pairing secrets are encrypted
// with DPAPI, so they are readable only by this Windows user.
#pragma once
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
};

class Store {
public:
    void load();
    std::optional<PairedDevice> findPaired(const ClientId& id);
    void addPaired(const PairedDevice& d);
    bool removePaired(const std::string& idHex);
    std::vector<PairedDevice> pairedList();

    Settings settings();
    void setSettings(const Settings& s);

private:
    void savePaired();
    void saveSettings();
    std::mutex mutex_;
    std::vector<PairedDevice> paired_;
    Settings settings_;
};

std::string toHex(const uint8_t* p, size_t n);
bool fromHex(const std::string& s, uint8_t* out, size_t n);

}  // namespace sd
