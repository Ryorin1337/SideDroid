#include "store.h"

#include <windows.h>
#include <dpapi.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "log.h"

namespace sd {

std::string toHex(const uint8_t* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

bool fromHex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != n * 2) return false;
    auto val = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < n; ++i) {
        int a = val(s[i * 2]), b = val(s[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = static_cast<uint8_t>(a << 4 | b);
    }
    return true;
}

static std::wstring pairedPath() { return dataDir() + L"\\paired.bin"; }
static std::wstring settingsPath() { return dataDir() + L"\\settings.ini"; }

static bool readFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// Write then rename, so a crash cannot leave a half-written file.
static bool writeFile(const std::wstring& path, const void* data, size_t size) {
    auto tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    fwrite(data, 1, size, f);
    fclose(f);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void Store::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    paired_.clear();
    std::string blob;
    if (readFile(pairedPath(), blob) && !blob.empty()) {
        DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data())}, out{};
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
            std::istringstream ss(std::string(reinterpret_cast<char*>(out.pbData), out.cbData));
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
            std::string idHex, secretHex, name;
            while (ss >> idHex >> secretHex && std::getline(ss, name)) {
                PairedDevice d;
                if (!name.empty() && name[0] == ' ') name.erase(0, 1);
                d.name = name;
                if (fromHex(idHex, d.id.data(), d.id.size()) && fromHex(secretHex, d.secret.data(), d.secret.size()))
                    paired_.push_back(d);
            }
        } else {
            logf("cannot decrypt paired devices; they will need pairing again");
        }
    }

    std::string text;
    if (readFile(settingsPath(), text)) {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = line.substr(0, eq);
            if (key.rfind("device.", 0) == 0) {
                DeviceSettings d;
                if (parseDeviceSettings(line.substr(eq + 1), d)) devices_[key.substr(7)] = d;
                continue;
            }
            if (key == "encoder" || key == "vddGpu") {
                std::string value = line.substr(eq + 1);
                if (!value.empty() && value.back() == '\r') value.pop_back();
                (key == "encoder" ? settings_.encoder : settings_.vddGpu) = value;
                continue;
            }
            long v = atol(line.c_str() + eq + 1);
            if (key == "width") settings_.width = static_cast<uint16_t>(v);
            else if (key == "height") settings_.height = static_cast<uint16_t>(v);
            else if (key == "hz") settings_.hz = static_cast<uint16_t>(v);
            else if (key == "maxKbps") settings_.maxKbps = static_cast<uint32_t>(v);
            else if (key == "allowWireless") settings_.allowWireless = v != 0;
            else if (key == "mirror") settings_.mirror = v != 0;
            else if (key == "scale") settings_.scalePercent = static_cast<uint16_t>(v);
            else if (key == "fill") settings_.fill = static_cast<uint8_t>(v >= 0 && v <= 3 ? v : 0);
            else if (key == "consoleAutostartAsked") settings_.consoleAutostartAsked = v != 0;
            else if (key == "vddLeft") settings_.vddLeft = static_cast<int>(v);
            else if (key == "vddTop") settings_.vddTop = static_cast<int>(v);
            else if (key == "vddRight") settings_.vddRight = static_cast<int>(v);
            else if (key == "vddBottom") settings_.vddBottom = static_cast<int>(v);
        }
    }
}

void Store::savePaired() {
    std::ostringstream ss;
    for (const auto& d : paired_) {
        std::string safe = d.name;
        for (auto& c : safe) if (c == '\n' || c == '\r') c = ' ';
        ss << toHex(d.id.data(), d.id.size()) << ' ' << toHex(d.secret.data(), d.secret.size()) << ' ' << safe << '\n';
    }
    std::string plain = ss.str();
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())}, out{};
    if (CryptProtectData(&in, L"SideDroid", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (!writeFile(pairedPath(), out.pbData, out.cbData)) logf("cannot save paired devices");
        LocalFree(out.pbData);
    } else {
        logf("CryptProtectData failed: %lu", GetLastError());
    }
    SecureZeroMemory(plain.data(), plain.size());
}

void Store::saveSettings() {
    std::ostringstream ss;
    ss << "width=" << settings_.width << "\nheight=" << settings_.height << "\nhz=" << settings_.hz
       << "\nmaxKbps=" << settings_.maxKbps << "\nallowWireless=" << (settings_.allowWireless ? 1 : 0) << "\nmirror=" << (settings_.mirror ? 1 : 0)
       << "\nscale=" << settings_.scalePercent << "\nfill=" << static_cast<int>(settings_.fill) << "\nencoder=" << settings_.encoder << "\nvddGpu=" << settings_.vddGpu << "\nconsoleAutostartAsked=" << (settings_.consoleAutostartAsked ? 1 : 0) << "\nvddLeft=" << settings_.vddLeft << "\nvddTop=" << settings_.vddTop
       << "\nvddRight=" << settings_.vddRight << "\nvddBottom=" << settings_.vddBottom << "\n";
    for (const auto& [id, d] : devices_) ss << "device." << id << '=' << serializeDeviceSettings(d) << '\n';
    auto s = ss.str();
    if (!writeFile(settingsPath(), s.data(), s.size())) logf("cannot save settings");
}

std::optional<PairedDevice> Store::findPaired(const ClientId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : paired_)
        if (d.id == id) return d;
    return std::nullopt;
}

void Store::addPaired(const PairedDevice& dev) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& d : paired_) {
        if (d.id == dev.id) {
            d = dev;
            savePaired();
            return;
        }
    }
    paired_.push_back(dev);
    savePaired();
}

bool Store::removePaired(const std::string& idHex) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = paired_.begin(); it != paired_.end(); ++it) {
        if (toHex(it->id.data(), it->id.size()) == idHex) {
            paired_.erase(it);
            savePaired();
            if (devices_.erase(idHex)) saveSettings();
            return true;
        }
    }
    return false;
}

std::vector<PairedDevice> Store::pairedList() {
    std::lock_guard<std::mutex> lock(mutex_);
    return paired_;
}

Settings Store::settings() {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void Store::setSettings(const Settings& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = s;
    saveSettings();
}

void copyDisplayDefaults(const Settings& s, DeviceSettings& into) {
    into.width = s.width;
    into.height = s.height;
    into.hz = s.hz;
    into.maxKbps = s.maxKbps;
    into.mirror = s.mirror;
    into.scalePercent = s.scalePercent;
    into.fill = s.fill;
    into.encoder = s.encoder;
}

std::string serializeDeviceSettings(const DeviceSettings& d) {
    std::ostringstream ss;
    ss << d.width << ',' << d.height << ',' << d.hz << ',' << d.maxKbps << ',' << (d.mirror ? 1 : 0) << ',' << d.scalePercent << ','
       << (d.custom ? 1 : 0) << ',' << d.left << ',' << d.top << ',' << d.right << ',' << d.bottom << ",fill=" << static_cast<int>(d.fill) << ',' << d.encoder;
    return ss.str();
}

bool parseDeviceSettings(const std::string& text, DeviceSettings& out) {
    unsigned w, h, hz, kbps, mirror, scale, custom;
    int l, t, r, b, used = 0;
    if (sscanf(text.c_str(), "%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d%n", &w, &h, &hz, &kbps, &mirror, &scale, &custom, &l, &t, &r, &b, &used) != 11) return false;
    // Last, because a GPU's name can hold anything. Earlier versions did not have it.
    out.encoder = text.size() > static_cast<size_t>(used) + 1 ? text.substr(used + 1) : std::string{};
    // Marked, because earlier versions have the encoder straight after the numbers.
    out.fill = 0;
    if (out.encoder.rfind("fill=", 0) == 0) {
        const int fill = atoi(out.encoder.c_str() + 5);
        out.fill = static_cast<uint8_t>(fill >= 0 && fill <= 3 ? fill : 0);
        const auto comma = out.encoder.find(',');
        out.encoder = comma == std::string::npos ? std::string{} : out.encoder.substr(comma + 1);
    }
    if (!out.encoder.empty() && out.encoder.back() == '\r') out.encoder.pop_back();
    out.width = static_cast<uint16_t>(w);
    out.height = static_cast<uint16_t>(h);
    out.hz = static_cast<uint16_t>(hz);
    out.maxKbps = kbps;
    out.mirror = mirror != 0;
    out.scalePercent = static_cast<uint16_t>(scale);
    out.custom = custom != 0;
    out.left = l;
    out.top = t;
    out.right = r;
    out.bottom = b;
    return true;
}

DeviceSettings Store::deviceSettings(const ClientId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    DeviceSettings d;
    const auto it = devices_.find(toHex(id.data(), id.size()));
    if (it != devices_.end()) {
        d = it->second;
    } else {
        d.left = settings_.vddLeft;
        d.top = settings_.vddTop;
        d.right = settings_.vddRight;
        d.bottom = settings_.vddBottom;
    }
    if (!d.custom) copyDisplayDefaults(settings_, d);
    return d;
}

void Store::setDeviceSettings(const ClientId& id, const DeviceSettings& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_[toHex(id.data(), id.size())] = d;
    saveSettings();
}

void Store::clearCustomDeviceSettings() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, d] : devices_) d.custom = false;
    saveSettings();
}

}  // namespace sd
