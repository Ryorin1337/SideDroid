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

static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\SideDroidHost";

void IpcServer::start() {
    running_ = true;
    thread_ = std::thread(&IpcServer::loop, this);
}

void IpcServer::stop() {
    if (!running_.exchange(false)) return;
    // Wake the blocking ConnectNamedPipe.
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (thread_.joinable()) thread_.join();
}

void IpcServer::loop() {
    // Only this user, and never over the network.
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    HANDLE token = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    std::vector<BYTE> tu(256);
    DWORD got = 0;
    LPWSTR sid = nullptr;
    if (token && GetTokenInformation(token, TokenUser, tu.data(), static_cast<DWORD>(tu.size()), &got) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tu.data())->User.Sid, &sid)) {
        std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
        LocalFree(sid);
    }
    if (token) CloseHandle(token);
    sa.lpSecurityDescriptor = sd;
    if (!sd) {
        // The default DACL would admit more than this user, so refuse to serve at all.
        logf("ipc: cannot build pipe security; console channel disabled");
        return;
    }

    bool first = true;
    while (running_) {
        DWORD mode = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE pipe = CreateNamedPipeW(kPipeName, mode,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                       65536, 65536, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            logf("ipc: CreateNamedPipe failed: %lu", GetLastError());
            if (first) break;  // another host instance owns the pipe
            Sleep(200);
            continue;
        }
        first = false;
        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        std::string pending;
        char buf[4096];
        while (connected && running_) {
            DWORD n = 0;
            if (!ReadFile(pipe, buf, sizeof(buf), &n, nullptr) || n == 0) break;
            pending.append(buf, n);
            if (pending.size() > 1024 * 1024) break;
            size_t nl;
            bool failed = false;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                std::string resp = handler_(line) + "\n";
                DWORD w = 0;
                if (!WriteFile(pipe, resp.data(), static_cast<DWORD>(resp.size()), &w, nullptr)) {
                    failed = true;
                    break;
                }
            }
            if (failed) break;
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    LocalFree(sd);
}

}  // namespace sd
