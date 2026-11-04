#include "ipc.h"

#include <windows.h>
#include <sddl.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "log.h"

namespace sd {

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o.push_back(static_cast<char>(c));
            }
        }
    }
    return o;
}

static size_t findValue(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    auto p = json.find(needle);
    if (p == std::string::npos) return std::string::npos;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return std::string::npos;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    return p < json.size() ? p : std::string::npos;
}

std::string jsonGetString(const std::string& json, const char* key) {
    auto p = findValue(json, key);
    if (p == std::string::npos || json[p] != '"') return {};
    std::string out;
    for (size_t i = p + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') return out;
        if (c == '\\' && i + 1 < json.size()) {
            char n = json[++i];
            out.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

long long jsonGetNumber(const std::string& json, const char* key, long long fallback) {
    auto p = findValue(json, key);
    if (p == std::string::npos) return fallback;
    char* end = nullptr;
    long long v = std::strtoll(json.c_str() + p, &end, 10);
    return end == json.c_str() + p ? fallback : v;
}

bool jsonGetBool(const std::string& json, const char* key, bool fallback) {
    auto p = findValue(json, key);
    if (p == std::string::npos) return fallback;
    if (json.compare(p, 4, "true") == 0) return true;
    if (json.compare(p, 5, "false") == 0) return false;
    return fallback;
}

void IpcServer::start() {
    // Only this user, and never over the network.
    HANDLE token = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    std::vector<BYTE> tu(256);
    DWORD got = 0;
    LPWSTR sid = nullptr;
    if (token && GetTokenInformation(token, TokenUser, tu.data(), static_cast<DWORD>(tu.size()), &got) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tu.data())->User.Sid, &sid)) {
        std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &securityDescriptor_, nullptr);
        LocalFree(sid);
    }
    if (token) CloseHandle(token);
    if (!securityDescriptor_) {
        // The default DACL would admit more than this user, so refuse to serve at all.
        logf("ipc: cannot build pipe security; console channel disabled");
        return;
    }
    securityAttributes_ = {sizeof(securityAttributes_), securityDescriptor_, FALSE};
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    for (int i = 0; i < kMaxClients; ++i) workers_.emplace_back(&IpcServer::worker, this, &securityAttributes_);
}

void IpcServer::stop() {
    if (!running_.exchange(false)) return;
    // Every wait below includes this event, so a connected console cannot hold up exit.
    SetEvent(stopEvent_);
    for (auto& t : workers_) t.join();
    workers_.clear();
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    LocalFree(securityDescriptor_);
    securityDescriptor_ = nullptr;
}

bool IpcServer::waitIo(HANDLE pipe, OVERLAPPED& ov, DWORD* transferred) {
    HANDLE events[2] = {ov.hEvent, stopEvent_};
    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &ov, &ignored, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, transferred, FALSE) != FALSE;
}

void IpcServer::serveClient(HANDLE pipe, HANDLE ioEvent) {
    std::string pending;
    char buf[4096];
    while (running_) {
        OVERLAPPED ov{};
        ov.hEvent = ioEvent;
        DWORD n = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &n, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING || !waitIo(pipe, ov, &n)) return;
        }
        if (n == 0) return;
        pending.append(buf, n);
        if (pending.size() > 1024 * 1024) return;
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            const std::string resp = handler_(line) + "\n";
            OVERLAPPED wov{};
            wov.hEvent = ioEvent;
            DWORD w = 0;
            if (!WriteFile(pipe, resp.data(), static_cast<DWORD>(resp.size()), &w, &wov)) {
                if (GetLastError() != ERROR_IO_PENDING || !waitIo(pipe, wov, &w)) return;
            }
        }
    }
}

void IpcServer::worker(SECURITY_ATTRIBUTES* sa) {
    const std::wstring path = LR"(\\.\pipe\)" + pipeName_;
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (running_) {
        HANDLE pipe = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       kMaxClients, 65536, 65536, 0, sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            logf("ipc: CreateNamedPipe failed: %lu", GetLastError());
            WaitForSingleObject(stopEvent_, 1000);
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = ioEvent;
        ResetEvent(ioEvent);
        bool connected = false;
        if (ConnectNamedPipe(pipe, &ov)) {
            connected = true;
        } else {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (e == ERROR_IO_PENDING) {
                DWORD ignored = 0;
                connected = waitIo(pipe, ov, &ignored);
            }
        }
        if (connected) serveClient(pipe, ioEvent);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    CloseHandle(ioEvent);
}

}  // namespace sd
