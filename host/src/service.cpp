// sidedroid-service: turns the virtual display device on and off for the host.
//
// The virtual displays should exist only while a device is connected. Changing the state of
// a device takes administrator rights and the host runs as the user, so this small service
// does it. It runs as SYSTEM, which is why it does so little: a client picks one of three
// fixed words, there are no paths or other parameters, and the only device it can reach is
// the one with the driver's hardware id. Worst case, a local program turns that display off.
//
// Installed and removed by the MSI.
#include <windows.h>
#include <sddl.h>

#include <cstdio>
#include <cstring>

#include "vdd_device.h"

namespace {

constexpr wchar_t kServiceName[] = L"SideDroidService";
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\SideDroidDisplayService";
// SYSTEM, and users logged on at this computer.
constexpr wchar_t kPipeSecurity[] = L"D:P(A;;GA;;;SY)(A;;GRGW;;;IU)";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;

void report(DWORD state, DWORD win32Exit = NO_ERROR) {
    SERVICE_STATUS s{};
    s.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s.dwCurrentState = state;
    s.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    s.dwWin32ExitCode = win32Exit;
    s.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 5000;
    SetServiceStatus(g_statusHandle, &s);
}

DWORD WINAPI onControl(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
    }
    return NO_ERROR;
}

DWORD perform(const char* word) {
    using sd::VddDeviceAction;
    // A driver the user installed for themselves can restart to read its configuration,
    // but whether it is on is their business.
    if (strcmp(word, "restart") == 0) return sd::changeVddDevice(VddDeviceAction::Restart);
    if (!sd::vddDriverIsOurs()) return ERROR_ACCESS_DENIED;
    if (strcmp(word, "enable") == 0) return sd::changeVddDevice(VddDeviceAction::Enable);
    if (strcmp(word, "disable") == 0) return sd::changeVddDevice(VddDeviceAction::Disable);
    return ERROR_INVALID_FUNCTION;
}

// Returns false to stop waiting, when the service is stopping.
bool waitIo(HANDLE pipe, OVERLAPPED& ov, DWORD* transferred) {
    HANDLE events[2] = {ov.hEvent, g_stopEvent};
    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &ov, &ignored, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, transferred, FALSE) != FALSE;
}

void serveClient(HANDLE pipe, HANDLE ioEvent) {
    char word[16]{};
    OVERLAPPED ov{};
    ov.hEvent = ioEvent;
    DWORD n = 0;
    if (!ReadFile(pipe, word, sizeof(word) - 1, &n, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING || !waitIo(pipe, ov, &n)) return;
    }
    word[n] = 0;
    char* end = strchr(word, '\n');
    if (end) *end = 0;
    char reply[32];
    const int len = snprintf(reply, sizeof(reply), "%lu\n", perform(word));
    OVERLAPPED wov{};
    wov.hEvent = ioEvent;
    DWORD w = 0;
    if (!WriteFile(pipe, reply, static_cast<DWORD>(len), &w, &wov) && GetLastError() == ERROR_IO_PENDING) waitIo(pipe, wov, &w);
    FlushFileBuffers(pipe);
}

void serve() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSecurity, SDDL_REVISION_1, &descriptor, nullptr)) return;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        // First instance only, so that nothing else can already be sitting on this name.
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 256, 256, 0, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(g_stopEvent, 1000);
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
            DWORD ignored = 0;
            connected = e == ERROR_PIPE_CONNECTED || (e == ERROR_IO_PENDING && waitIo(pipe, ov, &ignored));
        }
        if (connected) serveClient(pipe, ioEvent);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    CloseHandle(ioEvent);
    LocalFree(descriptor);
}

void WINAPI serviceMain(DWORD, wchar_t**) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, onControl, nullptr);
    if (!g_statusHandle) return;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    report(SERVICE_START_PENDING);
    // Nothing is connected this early, so no display should exist. It can be left on by a
    // power cut or a crash, and Windows would then show it until the host starts.
    if (sd::vddDriverIsOurs()) sd::changeVddDevice(sd::VddDeviceAction::Disable);
    report(SERVICE_RUNNING);
    serve();
    report(SERVICE_STOPPED);
}

}  // namespace

int wmain() {
    wchar_t name[] = L"SideDroidService";
    const SERVICE_TABLE_ENTRYW table[] = {{name, serviceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
