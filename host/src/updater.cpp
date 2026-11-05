#include "updater.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

#include "ipc.h"
#include "log.h"

namespace sd {

namespace {

constexpr auto kFirstCheckDelay = std::chrono::seconds(5);
constexpr auto kCheckInterval = std::chrono::hours(12);
constexpr auto kRetryInterval = std::chrono::hours(1);
constexpr size_t kMaxManifestBytes = 64 * 1024;
constexpr size_t kMaxNotesBytes = 4000;

std::wstring updatesDir() { return dataDir() + L"\\updates"; }

std::optional<std::string> sha256OfFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::nullopt;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::optional<std::string> result;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<BYTE> buf(256 * 1024);
        bool ok = true;
        size_t n;
        while (ok && (n = fread(buf.data(), 1, buf.size(), f)) > 0)
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), static_cast<ULONG>(n), 0));
        BYTE digest[32];
        if (ok && !ferror(f) && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            std::string hex;
            char b[3];
            for (BYTE d : digest) {
                snprintf(b, sizeof(b), "%02x", d);
                hex += b;
            }
            result = hex;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    return result;
}

void deleteDownloads() {
    WIN32_FIND_DATAW fd;
    const std::wstring dir = updatesDir();
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

bool isValidVersion(const std::string& version) {
    int parts = 1;
    bool digit = false;
    for (char c : version) {
        if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c == '.' && digit && parts < 4) {
            digit = false;
            ++parts;
        } else {
            return false;
        }
    }
    return digit && version.size() <= 32;
}

int compareVersions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        const auto next = [](const std::string& s, size_t& p) {
            long long v = 0;
            for (; p < s.size() && s[p] != '.'; ++p) v = v * 10 + (s[p] - '0');
            ++p;
            return v;
        };
        const long long x = next(a, i), y = next(b, j);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::optional<UpdateManifest> parseUpdateManifest(const std::string& json) {
    UpdateManifest m;
    m.version = jsonGetString(json, "version");
    m.notes = jsonGetString(json, "notes");
    m.msiSha256 = jsonGetString(json, "msi");
    if (!isValidVersion(m.version) || m.msiSha256.size() != 64) return std::nullopt;
    for (char& c : m.msiSha256) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
    }
    if (m.notes.size() > kMaxNotesBytes) {
        // Not in the middle of a character.
        size_t cut = kMaxNotesBytes;
        while (cut > 0 && (static_cast<unsigned char>(m.notes[cut]) & 0xC0) == 0x80) --cut;
        m.notes.resize(cut);
    }
    return m;
}

void Updater::start() {
    thread_ = std::thread(&Updater::run, this);
}

void Updater::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        // Closing the handle ends a blocking call on the other thread right away.
        if (request_) {
            WinHttpCloseHandle(request_);
            request_ = nullptr;
        }
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

UpdateStatus Updater::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool Updater::install() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!manifest_ || (status_.state != "available" && status_.state != "failed")) return false;
        installRequested_ = true;
        status_.state = "downloading";
        status_.progress = 0;
    }
    wake_.notify_all();
    return true;
}

void Updater::setState(const char* state, int progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = state;
    status_.progress = progress;
}

void Updater::run() {
    auto next = std::chrono::steady_clock::now() + kFirstCheckDelay;
    while (true) {
        bool installNow = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_until(lock, next, [&] { return stopping_ || installRequested_; });
            if (stopping_) return;
            installNow = installRequested_;
            installRequested_ = false;
        }
        if (installNow) {
            downloadAndInstall();
        } else {
            next = std::chrono::steady_clock::now() + (check() ? kCheckInterval : kRetryInterval);
        }
    }
}

bool Updater::httpGet(const std::wstring& url, const std::function<bool(const BYTE*, DWORD, uint64_t)>& sink) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = _countof(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;

    HINTERNET session = WinHttpOpen(L"SideDroid-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    const DWORD secure = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure) : nullptr;
    bool ok = false;
    if (request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                WinHttpCloseHandle(request);
                request = nullptr;
            } else {
                request_ = request;
            }
        }
    }
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            wchar_t lengthText[32] = {};
            DWORD lengthSize = sizeof(lengthText);
            uint64_t total = 0;
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize, WINHTTP_NO_HEADER_INDEX))
                total = _wcstoui64(lengthText, nullptr, 10);
            std::vector<BYTE> buf(64 * 1024);
            while (true) {
                DWORD got = 0;
                if (!WinHttpReadData(request, buf.data(), static_cast<DWORD>(buf.size()), &got)) break;
                if (got == 0) {
                    ok = true;
                    break;
                }
                if (!sink(buf.data(), got, total)) break;
            }
        } else {
            logf("update: HTTP %lu for %s", code, narrow(url).c_str());
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_) {
            WinHttpCloseHandle(request_);
            request_ = nullptr;
        }
    }
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool Updater::check() {
    std::string body;
    const bool got = httpGet(config_.manifestUrl, [&](const BYTE* p, DWORD n, uint64_t) {
        if (body.size() + n > kMaxManifestBytes) return false;
        body.append(reinterpret_cast<const char*>(p), n);
        return true;
    });
    if (!got) {
        logf("update check failed: %lu", GetLastError());
        return false;
    }
    auto manifest = parseUpdateManifest(body);
    if (!manifest) {
        logf("update check: the manifest is not usable");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (compareVersions(manifest->version, config_.currentVersion) <= 0) {
        manifest_.reset();
        status_ = UpdateStatus{};
        // Whatever is left of an earlier update has done its job.
        deleteDownloads();
        return true;
    }
    if (status_.version != manifest->version) {
        logf("update available: %s", manifest->version.c_str());
        status_ = UpdateStatus{};
        status_.state = "available";
        status_.version = manifest->version;
        status_.notes = manifest->notes;
    }
    manifest_ = std::move(manifest);
    return true;
}

bool Updater::download(const std::wstring& url, const std::wstring& path) {
    const std::wstring part = path + L".part";
    FILE* f = _wfopen(part.c_str(), L"wb");
    if (!f) {
        logf("update: cannot write %s", narrow(part).c_str());
        return false;
    }
    uint64_t done = 0;
    int shown = -1;
    const bool ok = httpGet(url, [&](const BYTE* p, DWORD n, uint64_t total) {
        if (fwrite(p, 1, n, f) != n) return false;
        done += n;
        const int percent = total ? static_cast<int>(done * 100 / total) : 0;
        if (percent != shown) {
            shown = percent;
            setState("downloading", percent);
        }
        return true;
    });
    const bool closed = fclose(f) == 0;
    if (!ok || !closed || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(part.c_str());
        return false;
    }
    return true;
}

void Updater::downloadAndInstall() {
    UpdateManifest m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!manifest_) return;
        m = *manifest_;
    }
    CreateDirectoryW(updatesDir().c_str(), nullptr);
    const std::wstring version = widen(m.version);
    const std::wstring fileName = L"SideDroid-" + version + L"-x64.msi";
    const std::wstring path = updatesDir() + L"\\" + fileName;

    // A finished download from an earlier attempt saves a slow connection the repeat.
    if (sha256OfFile(path) != m.msiSha256) {
        logf("update: downloading %s", m.version.c_str());
        if (!download(config_.releaseBase + L"/v" + version + L"/" + fileName, path)) {
            logf("update: download failed");
            setState("failed");
            return;
        }
        if (sha256OfFile(path) != m.msiSha256) {
            logf("update: the download does not match its checksum");
            DeleteFileW(path.c_str());
            setState("failed");
            return;
        }
    }

    setState("installing");
    if (!config_.launchInstaller) {
        logf("update: verified %s; a test run does not install it", narrow(path).c_str());
        return;
    }

    // The installer stops this host, so it has to outlive it. It asks for elevation itself.
    const std::wstring args = L"/i \"" + path + L"\" /passive /norestart";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"msiexec.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        logf("update: cannot start the installer: %lu", GetLastError());
        setState("failed");
        return;
    }
    // Still here when it ends means it did not go through, usually a declined prompt.
    while (WaitForSingleObject(sei.hProcess, 500) == WAIT_TIMEOUT) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) break;
    }
    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != STILL_ACTIVE) {
        logf("update: the installer ended with %lu", code);
        setState("available");
    }
}

}  // namespace sd
