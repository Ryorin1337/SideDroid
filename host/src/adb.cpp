#include "adb.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

#include "log.h"

namespace sd {

std::vector<AdbDevice> parseAdbDevices(const std::string& output) {
    std::vector<AdbDevice> out;
    std::istringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Daemon chatter starts with "*", and the header has no tab.
        if (line.empty() || line[0] == '*') continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        out.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
    return out;
}

static std::wstring adbPath() {
    // The copy installed beside the host comes first.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    dir.erase(dir.find_last_of(L'\\'));
    auto bundled = dir + L"\\adb\\adb.exe";
    if (GetFileAttributesW(bundled.c_str()) != INVALID_FILE_ATTRIBUTES) return bundled;
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

// Runs hidden, returns stdout, and kills the process at the deadline.
static bool runCapture(const std::wstring& exe, const std::wstring& args, std::string* out, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // Named explicitly: adb may leave a server running for hours, and whatever
    // it inherits stays open that long.
    SIZE_T listSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
    std::vector<char> listBuf(listSize);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(listBuf.data());
    InitializeProcThreadAttributeList(list, 1, 0, &listSize);
    HANDLE inherited[1] = {wr};
    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.lpAttributeList = list;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                             nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(list);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    std::string text;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool finished = false;
    while (GetTickCount64() < deadline) {
        DWORD avail = 0;
        BOOL peek = PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr);
        if (avail > 0) {
            char buf[4096];
            DWORD n = 0;
            if (ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) && n) text.append(buf, n);
            continue;
        }
        // The pipe closes when adb exits, but a started daemon may inherit it,
        // so the process handle decides whether we are done.
        if (!peek || WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            finished = true;
            break;
        }
    }
    if (!finished) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (out) *out = std::move(text);
    return finished && code == 0;
}

void AdbWatcher::killServer() {
    const auto adb = adbPath();
    if (!adb.empty()) runCapture(adb, L"kill-server", nullptr, 8000);
}

void AdbWatcher::start() {
    running_ = true;
    thread_ = std::thread(&AdbWatcher::loop, this);
}

void AdbWatcher::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::vector<AdbDevice> AdbWatcher::devices() {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

void AdbWatcher::loop() {
    auto adb = adbPath();
    found_ = !adb.empty();
    if (adb.empty()) {
        logf("adb not found; USB connections unavailable");
        return;
    }
    while (running_) {
        std::string text;
        if (runCapture(adb, L"devices", &text, 8000)) {
            auto devs = parseAdbDevices(text);
            // A device that went away must be set up again when it returns.
            reversed_.erase(std::remove_if(reversed_.begin(), reversed_.end(),
                                           [&](const std::string& s) {
                                               return std::none_of(devs.begin(), devs.end(), [&](const AdbDevice& d) {
                                                   return d.serial == s && d.state == "device";
                                               });
                                           }),
                            reversed_.end());
            for (const auto& d : devs) {
                if (d.state != "device") continue;
                if (std::find(reversed_.begin(), reversed_.end(), d.serial) != reversed_.end()) continue;
                auto args = L"-s " + widen(d.serial) + L" reverse tcp:" + std::to_wstring(port_) + L" tcp:" + std::to_wstring(port_);
                if (runCapture(adb, args, nullptr, 8000)) {
                    logf("adb reverse set for %s", d.serial.c_str());
                    reversed_.push_back(d.serial);
                }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            devices_ = std::move(devs);
        }
        for (int i = 0; i < 20 && running_; ++i) Sleep(100);
    }
}

}  // namespace sd
