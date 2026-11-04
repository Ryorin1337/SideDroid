// sidedroid-host: runs in the tray. The console is a separate process that talks
// to it over a pipe, so closing the console never interrupts the stream.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

#include "display.h"
#include "host.h"
#include "log.h"
#include "resource.h"

using namespace sd;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT ID_OPEN = 1, ID_EXIT = 2;
Host* g_host = nullptr;

std::wstring exeDir() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    dir.erase(dir.find_last_of(L'\\'));
    return dir;
}

void openConsole() {
    const auto path = exeDir() + L"\\console\\SideDroid.exe";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        logf("cannot start console at %s", narrow(path).c_str());
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            openConsole();
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_OPEN, L"\x6253\x5F00\x63A7\x5236\x53F0");  // 打开控制台
            AppendMenuW(menu, MF_STRING, ID_EXIT, L"\x9000\x51FA");                    // 退出
            SetForegroundWindow(hwnd);
            UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == ID_OPEN) openConsole();
            if (cmd == ID_EXIT) PostQuitMessage(0);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int runTray(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SideDroidHostTray";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SideDroid", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    lstrcpynW(nid.szTip, L"SideDroid", ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    return 0;
}

BOOL WINAPI onConsoleCtrl(DWORD) {
    // Headless runs have no message window to post to.
    PostQuitMessage(0);
    ExitProcess(0);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    HostOptions opts;
    bool headless = false, listOnly = false;
    int runSeconds = 0;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--source" && i + 1 < argc) opts.sourceOverride = argv[++i];
        else if (a == L"--port" && i + 1 < argc) opts.port = static_cast<uint16_t>(_wtoi(argv[++i]));
        else if (a == L"--headless") headless = true;
        else if (a == L"--seconds" && i + 1 < argc) runSeconds = _wtoi(argv[++i]);
        else if (a == L"--no-adb") opts.enableAdb = false;
        else if (a == L"--no-ipc") opts.enableIpc = false;
        else if (a == L"--no-touch") opts.enableTouch = false;
        else if (a == L"--no-discovery") opts.enableDiscovery = false;
        else if (a == L"--test-pattern") opts.testPattern = true;
        else if (a == L"--list-outputs") listOnly = true;
    }
    LocalFree(argv);

    // Physical pixels everywhere: capture rectangles and injected touch must agree.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (headless || listOnly) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f;
            freopen_s(&f, "CONOUT$", "w", stderr);
            freopen_s(&f, "CONOUT$", "w", stdout);
        }
    }

    if (listOnly) {
        for (const auto& o : listOutputs()) {
            printf("%s attached=%d primary=%d vdd=%d rect=%ld,%ld,%ld,%ld id=%s\n", narrow(o.deviceName).c_str(),
                   o.attached, o.primary, o.isVdd, o.rect.left, o.rect.top, o.rect.right, o.rect.bottom,
                   narrow(o.monitorId).c_str());
        }
        fflush(stdout);
        return 0;
    }

    logInit(headless);

    // One tray host per user. Test runs on other ports may coexist.
    HANDLE single = CreateMutexW(nullptr, TRUE, (L"Local\\SideDroidHost-" + std::to_wstring(opts.port)).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!headless) openConsole();
        return 0;
    }

    Host host(opts);
    g_host = &host;
    if (!host.start()) {
        CloseHandle(single);
        return 1;
    }

    int rc = 0;
    if (headless) {
        SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
        for (int t = 0; runSeconds == 0 || t < runSeconds * 10; ++t) Sleep(100);
    } else {
        rc = runTray(inst);
    }
    host.stop();
    CloseHandle(single);
    CoUninitialize();
    return rc;
}
