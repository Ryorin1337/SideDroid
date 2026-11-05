// Checks update.json in the repository, and on the user's say-so downloads the MSI
// from that version's GitHub release, checks it, and hands it to Windows Installer.
#pragma once
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace sd {

// Pure, for testing.
//
// Negative, zero or positive as a is older than, equal to or newer than b.
int compareVersions(const std::string& a, const std::string& b);
// One to four dot separated numbers. Versions end up in URLs and file names, so
// this is what keeps anything else out of them.
bool isValidVersion(const std::string& version);

struct UpdateManifest {
    std::string version;
    std::string notes;
    std::string msiSha256;  // lower case hex
};
// Nothing unless the version and the checksum are well formed.
std::optional<UpdateManifest> parseUpdateManifest(const std::string& json);

struct UpdateStatus {
    // none, available, downloading, installing or failed
    std::string state = "none";
    std::string version;
    std::string notes;
    int progress = 0;  // percent, while downloading
};

class Updater {
public:
    struct Config {
        std::string currentVersion;
        std::wstring manifestUrl;
        // <releaseBase>/v<version>/SideDroid-<version>-x64.msi
        std::wstring releaseBase;
        bool launchInstaller = true;  // test runs stop short of a real installation
    };

    explicit Updater(Config config) : config_(std::move(config)) {}
    ~Updater() { stop(); }
    void start();
    void stop();
    UpdateStatus status();
    // Downloads and installs what the last check found. False when there is nothing to do.
    bool install();

private:
    void run();
    bool check();
    void downloadAndInstall();
    bool download(const std::wstring& url, const std::wstring& path);
    bool httpGet(const std::wstring& url, const std::function<bool(const BYTE*, DWORD, uint64_t)>& sink);
    void setState(const char* state, int progress = 0);

    Config config_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool installRequested_ = false;
    std::optional<UpdateManifest> manifest_;
    UpdateStatus status_;
    void* request_ = nullptr;  // the request in progress, closed to cancel it
};

}  // namespace sd
