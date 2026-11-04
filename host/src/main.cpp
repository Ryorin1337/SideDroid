// sidedroid-host: runs in the tray. The console is a separate process that talks
// to it over a pipe, so closing the console never interrupts the stream.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "display.h"
#include "host.h"
#include "log.h"
#include "resource.h"

using namespace sd;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kTooltipTimer = 1;
constexpr UINT kTooltipIntervalMs = 2000;

enum MenuId : UINT {
    ID_OPEN = 1, ID_EXIT, ID_DISCONNECT, ID_WIRELESS, ID_PAIR, ID_INSTALL_DRIVER,
    ID_MODE_EXTEND = 10, ID_MODE_MIRROR,
    ID_RES_PHONE = 20, ID_RES_PC,
    ID_RES_BASE = 30,      // up to 40 sizes
    ID_SCALE_BASE = 80,    // up to 20 steps
    ID_HZ_BASE = 100,      // up to 20 rates
    ID_QUALITY_BASE = 120,
};

struct QualityChoice { const wchar_t* label; uint32_t kbps; };
const QualityChoice kQualities[] = {{L"自适应", 0}, {L"省流", 10000}, {L"均衡", 25000}, {L"高清", 50000}};

Host* g_host = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbarCreated = 0;

std::wstring exeDir() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    dir.erase(dir.find_last_of(L'\\'));
    return dir;
}

void openConsole() {
    // Whatever goes wrong elsewhere, a burst of clicks or messages cannot pile up processes.
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (last != 0 && now - last < 2000) return;
    last = now;
    const auto path = exeDir() + L"\\console\\SideDroid.exe";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        logf("cannot start console at %s", narrow(path).c_str());
}

std::wstring format(const wchar_t* fmt, ...) {
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring errorLine(const std::string& code) {
    if (code == "virtual_display_missing") return L"未找到虚拟显示驱动";
    if (code == "virtual_display_config_denied") return L"无法写入虚拟显示配置";
    if (code == "virtual_display_layout") return L"无法把虚拟屏设为独立显示器";
    if (code == "no_encoder") return L"没有可用的硬件编码器";
    if (code == "port_in_use") return L"端口 27183 被其他程序占用，正在重试";
    return widen(code);
}

// The lines at the top of the menu, also used for the tooltip.
std::vector<std::wstring> statusLines() {
    std::vector<std::wstring> out;
    const LiveStatus l = g_host->liveStatus();
    if (!l.connected) {
        out.push_back(l.error.empty() ? L"未连接手机" : errorLine(l.error));
        return out;
    }
    out.push_back(format(L"已连接：%s · %s", widen(l.device).c_str(), l.transport == "wifi" ? L"Wi-Fi" : L"USB"));
    if (!l.error.empty()) {
        out.push_back(errorLine(l.error));
        return out;
    }
    if (l.displayW > 0) {
        out.push_back(format(L"%s · %d×%d · %d%% · %.0f fps", l.usingVirtualDisplay ? L"副屏" : L"镜像", l.displayW, l.displayH, l.usingVirtualDisplay ? l.currentScale : 100, l.measuredFps));
        out.push_back(format(L"%.1f Mbps · 延迟 %u ms · %s %s", l.kbps / 1000.0, l.rttMs, widen(l.encoder).c_str(), widen(l.codec).c_str()));
    }
    return out;
}

void updateTooltip() {
    std::wstring tip = L"SideDroid";
    const auto lines = statusLines();
    for (size_t i = 0; i < lines.size() && i < 2; ++i) tip += L"\n" + lines[i];
    lstrcpynW(g_nid.szTip, tip.c_str(), ARRAYSIZE(g_nid.szTip));
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void addRadio(HMENU menu, UINT id, const wchar_t* label, bool checked, bool enabled = true) {
    UINT flags = MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED);
    AppendMenuW(menu, flags, id, label);
}

void showMenu(HWND hwnd) {
    const Settings s = g_host->currentSettings();
    const LiveStatus l = g_host->liveStatus();
    HMENU menu = CreatePopupMenu();

    for (const auto& line : statusLines()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
    if (!vddInstalled()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"未安装虚拟显示驱动");
        AppendMenuW(menu, MF_STRING, ID_INSTALL_DRIVER, L"安装驱动…");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU modeMenu = CreatePopupMenu();
    addRadio(modeMenu, ID_MODE_EXTEND, L"副屏（扩展桌面）", !s.mirror);
    addRadio(modeMenu, ID_MODE_MIRROR, L"镜像（显示主屏）", s.mirror);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modeMenu), s.mirror ? L"模式：镜像" : L"模式：副屏");

    HMENU resMenu = CreatePopupMenu();
    const Size pc = primaryDisplaySize();
    const std::wstring phoneLabel = l.phoneW ? format(L"自适应手机（%d×%d）（推荐）", l.phoneW, l.phoneH) : L"自适应手机（推荐）";
    addRadio(resMenu, ID_RES_PHONE, phoneLabel.c_str(), s.width == 0 && s.height == 0);
    addRadio(resMenu, ID_RES_PC, format(L"自适应电脑（%d×%d）", pc.width, pc.height).c_str(), s.width == kResolutionMatchPc);
    AppendMenuW(resMenu, MF_SEPARATOR, 0, nullptr);
    const std::vector<Size> sizes = commonResolutions(l.maxDecodeLong, l.maxDecodeShort);
    for (UINT i = 0; i < sizes.size() && i < 40; ++i)
        addRadio(resMenu, ID_RES_BASE + i, format(L"%d×%d", sizes[i].width, sizes[i].height).c_str(), s.width == sizes[i].width && s.height == sizes[i].height);
    // Mirroring always shows the main display at its own resolution.
    AppendMenuW(menu, MF_POPUP | (s.mirror ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(resMenu), L"分辨率");

    HMENU scaleMenu = CreatePopupMenu();
    const auto& steps = scaleSteps();
    for (UINT i = 0; i < steps.size(); ++i) {
        const bool recommended = l.recommendedScale == steps[i];
        const std::wstring label = recommended ? format(L"%d%%（推荐）", steps[i]) : format(L"%d%%", steps[i]);
        const uint16_t shown = s.scalePercent ? s.scalePercent : l.recommendedScale;
        addRadio(scaleMenu, ID_SCALE_BASE + i, label.c_str(), shown == steps[i]);
    }
    AppendMenuW(menu, MF_POPUP | (s.mirror ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(scaleMenu), L"缩放");

    HMENU hzMenu = CreatePopupMenu();
    // A phone that has connected says what it can show. Until then, the usual ones.
    std::vector<uint16_t> rates = l.phoneRates.empty() ? std::vector<uint16_t>{60, 90, 120} : l.phoneRates;
    addRadio(hzMenu, ID_HZ_BASE, L"最高（推荐）", s.hz == 0);
    for (UINT i = 0; i < rates.size() && i < 18; ++i)
        addRadio(hzMenu, ID_HZ_BASE + 1 + i, format(L"%d Hz", rates[i]).c_str(), s.hz == rates[i]);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hzMenu), L"刷新率");

    HMENU qualityMenu = CreatePopupMenu();
    for (UINT i = 0; i < _countof(kQualities); ++i) addRadio(qualityMenu, ID_QUALITY_BASE + i, kQualities[i].label, s.maxKbps == kQualities[i].kbps);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(qualityMenu), L"画质");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    addRadio(menu, ID_WIRELESS, L"允许无线连接", s.allowWireless);
    AppendMenuW(menu, MF_STRING | (s.allowWireless ? 0 : MF_GRAYED), ID_PAIR, L"添加设备…");
    AppendMenuW(menu, MF_STRING | (l.connected ? 0 : MF_GRAYED), ID_DISCONNECT, L"断开连接");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_OPEN, L"打开控制台");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"退出");
    SetMenuDefaultItem(menu, ID_OPEN, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    // Without this pair the menu would stay open after clicking elsewhere.
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenus

    Settings next = s;
    if (cmd == ID_OPEN) {
        openConsole();
    } else if (cmd == ID_EXIT) {
        PostQuitMessage(0);
    } else if (cmd == ID_DISCONNECT) {
        g_host->disconnectPhone();
    } else if (cmd == ID_INSTALL_DRIVER) {
        g_host->handleCommand(R"({"cmd":"install_driver"})");
    } else if (cmd == ID_PAIR) {
        // The code is shown in the console, along with its countdown.
        g_host->startPairing();
        openConsole();
    } else if (cmd == ID_WIRELESS) {
        next.allowWireless = !s.allowWireless;
        g_host->applySettings(next);
    } else if (cmd == ID_MODE_EXTEND || cmd == ID_MODE_MIRROR) {
        next.mirror = cmd == ID_MODE_MIRROR;
        g_host->applySettings(next);
    } else if (cmd == ID_RES_PHONE || cmd == ID_RES_PC) {
        next.width = next.height = cmd == ID_RES_PC ? kResolutionMatchPc : 0;
        g_host->applySettings(next);
    } else if (cmd >= ID_RES_BASE && cmd < ID_RES_BASE + sizes.size()) {
        next.width = sizes[cmd - ID_RES_BASE].width;
        next.height = sizes[cmd - ID_RES_BASE].height;
        g_host->applySettings(next);
    } else if (cmd >= ID_SCALE_BASE && cmd < ID_SCALE_BASE + steps.size()) {
        const uint16_t chosen = steps[cmd - ID_SCALE_BASE];
        next.scalePercent = chosen == l.recommendedScale ? 0 : chosen;
        g_host->applySettings(next);
    } else if (cmd == ID_HZ_BASE) {
        next.hz = 0;
        g_host->applySettings(next);
    } else if (cmd > ID_HZ_BASE && cmd <= ID_HZ_BASE + rates.size()) {
        next.hz = rates[cmd - ID_HZ_BASE - 1];
        g_host->applySettings(next);
    } else if (cmd >= ID_QUALITY_BASE && cmd < ID_QUALITY_BASE + _countof(kQualities)) {
        next.maxKbps = kQualities[cmd - ID_QUALITY_BASE].kbps;
        g_host->applySettings(next);
    }
    updateTooltip();
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        // Explorer restarted, and with it the notification area.
        g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) openConsole();
        else if (LOWORD(lp) == WM_RBUTTONUP) showMenu(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == kTooltipTimer) updateTooltip();
        return 0;
    case WM_ENDSESSION:
        // Windows is shutting down: put the desktop back while there is still time.
        if (wp && g_host) g_host->stop();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int runTray(HINSTANCE inst) {
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SideDroidHostTray";
    RegisterClassW(&wc);
    // A real but never shown window: a message-only one cannot take the foreground,
    // and a tray menu needs that in order to close when the user clicks away.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"SideDroid", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    lstrcpynW(g_nid.szTip, L"SideDroid", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    SetTimer(hwnd, kTooltipTimer, kTooltipIntervalMs, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    KillTimer(hwnd, kTooltipTimer);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    return 0;
}

// A console window asking the process to go away must still put the desktop back.
BOOL WINAPI onConsoleCtrl(DWORD) {
    if (g_host) g_host->stop();
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
        else if (a == L"--test-virtual-display") opts.useVirtualDisplayInTest = true;
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
            printf("%s attached=%d primary=%d vdd=%d shared=%d rect=%ld,%ld,%ld,%ld id=%s\n", narrow(o.deviceName).c_str(),
                   o.attached, o.primary, o.isVdd, o.isVddShared, o.rect.left, o.rect.top, o.rect.right, o.rect.bottom,
                   narrow(o.monitorId).c_str());
        }
        fflush(stdout);
        return 0;
    }

    // Test runs on other ports keep their own log, so they cannot overwrite the real one.
    logInit(headless, opts.port == kDefaultPort ? L"host.log" : L"host-test.log");

    // One tray host per user. Test runs on other ports may coexist.
    HANDLE single = CreateMutexW(nullptr, TRUE, (L"Local\\SideDroidHost-" + std::to_wstring(opts.port)).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Only exit. The console starts a host when it needs one, so if a duplicate
        // host opened a console in return, the two could start each other forever.
        return 0;
    }

    Host host(opts);
    g_host = &host;
    if (!host.start()) {
        CloseHandle(single);
        return 1;
    }

    int rc = 0;
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
    if (headless) {
        for (int t = 0; runSeconds == 0 || t < runSeconds * 10; ++t) Sleep(100);
    } else {
        rc = runTray(inst);
    }
    host.stop();
    g_host = nullptr;
    CloseHandle(single);
    CoUninitialize();
    return rc;
}
