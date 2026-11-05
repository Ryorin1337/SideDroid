#include "vdd_device.h"

#include <setupapi.h>

namespace sd {

namespace {

constexpr wchar_t kHardwareId[] = L"Root\\MttVDD";
constexpr wchar_t kMarker[] = L"C:\\ProgramData\\SideDroid\\driver-installed-by-sidedroid";

bool isVddDevice(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    wchar_t buf[1024]{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(buf),
                                           sizeof(buf) - sizeof(wchar_t) * 2, nullptr))
        return false;
    for (const wchar_t* p = buf; *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, kHardwareId) == 0) return true;
    return false;
}

}  // namespace

bool vddDeviceExists() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    bool found = false;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); ++i) found = isVddDevice(set, dev);
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

bool vddDriverIsOurs() { return GetFileAttributesW(kMarker) != INVALID_FILE_ATTRIBUTES; }

DWORD changeVddDevice(VddDeviceAction action) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return GetLastError();
    SP_DEVINFO_DATA dev{sizeof(dev)};
    DWORD result = ERROR_NOT_FOUND;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        if (!isVddDevice(set, dev)) continue;
        SP_PROPCHANGE_PARAMS params{};
        params.ClassInstallHeader.cbSize = sizeof(params.ClassInstallHeader);
        params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        params.StateChange = action == VddDeviceAction::Enable ? DICS_ENABLE : action == VddDeviceAction::Disable ? DICS_DISABLE : DICS_PROPCHANGE;
        params.Scope = DICS_FLAG_GLOBAL;  // survives a restart of the computer
        const bool ok = SetupDiSetClassInstallParamsW(set, &dev, &params.ClassInstallHeader, sizeof(params)) &&
                        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev);
        result = ok ? ERROR_SUCCESS : GetLastError();
        // Reported as success, but it did not happen: something still has the device open, and
        // Windows has put it off until the next start of the computer.
        if (ok) {
            SP_DEVINSTALL_PARAMS_W install{};
            install.cbSize = sizeof(install);
            if (SetupDiGetDeviceInstallParamsW(set, &dev, &install) && (install.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)))
                result = ERROR_SUCCESS_RESTART_REQUIRED;
        }
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

}  // namespace sd
