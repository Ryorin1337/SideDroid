// sidedroid-driver-tool: install | uninstall | status
//
// Creates the Root\MttVDD device and installs the unmodified Virtual Display
// Driver package for it. Removal only touches a driver that this tool installed:
// a VDD the user already had stays exactly as it was.
//
// No certificate is imported. Windows shows its own publisher prompt, so the
// decision to trust the driver stays with the user.
#include <windows.h>
#include <aclapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "cfgmgr32.lib")

namespace {

constexpr wchar_t kHardwareId[] = L"Root\\MttVDD";
constexpr wchar_t kConfigDir[] = L"C:\\VirtualDisplayDriver";
constexpr wchar_t kMarkerDir[] = L"C:\\ProgramData\\SideDroid";
constexpr wchar_t kMarker[] = L"C:\\ProgramData\\SideDroid\\driver-installed-by-sidedroid";

void fail(const char* what) {
    fprintf(stderr, "%s failed: %lu\n", what, GetLastError());
}

bool hasHardwareId(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    wchar_t buf[1024]{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(buf),
                                           sizeof(buf) - sizeof(wchar_t) * 2, nullptr))
        return false;
    for (const wchar_t* p = buf; *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, kHardwareId) == 0) return true;
    return false;
}

bool deviceExists() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    bool found = false;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); ++i) found = hasHardwareId(set, dev);
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// The host edits this file to add a phone's modes, without running elevated.
bool prepareConfig(const std::wstring& packageDir) {
    CreateDirectoryW(kConfigDir, nullptr);
    std::wstring cfg = std::wstring(kConfigDir) + L"\\vdd_settings.xml";
    if (GetFileAttributesW(cfg.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring src = packageDir + L"\\vdd_settings.xml";
        if (!CopyFileW(src.c_str(), cfg.c_str(), TRUE)) {
            fail("copy vdd_settings.xml");
            return false;
        }
    }
    PSID users = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &users))
        return false;
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(users);
    PACL oldAcl = nullptr, newAcl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool ok = false;
    if (GetNamedSecurityInfoW(kConfigDir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldAcl, nullptr, &sd) == ERROR_SUCCESS &&
        SetEntriesInAclW(1, &ea, oldAcl, &newAcl) == ERROR_SUCCESS) {
        wchar_t dir[MAX_PATH];
        lstrcpynW(dir, kConfigDir, MAX_PATH);
        ok = SetNamedSecurityInfoW(dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl, nullptr) == ERROR_SUCCESS;
    }
    if (newAcl) LocalFree(newAcl);
    if (sd) LocalFree(sd);
    FreeSid(users);
    if (!ok) fprintf(stderr, "could not open %ls to the host process\n", kConfigDir);
    return ok;
}

int install(const std::wstring& packageDir) {
    std::wstring inf = packageDir + L"\\MttVDD.inf";
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "driver package not found at %ls\n", packageDir.c_str());
        return 2;
    }
    if (!prepareConfig(packageDir)) return 3;

    if (deviceExists()) {
        // Already installed by the user, or by an earlier run: keep it, keep ownership as is.
        printf("driver already present\n");
        return 0;
    }

    GUID cls;
    wchar_t className[64];
    if (!SetupDiGetINFClassW(inf.c_str(), &cls, className, ARRAYSIZE(className), nullptr)) {
        fail("SetupDiGetINFClass");
        return 4;
    }
    HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        fail("SetupDiCreateDeviceInfoList");
        return 4;
    }
    SP_DEVINFO_DATA dev{sizeof(dev)};
    wchar_t ids[64] = L"Root\\MttVDD";  // double-null terminated by the zero fill
    bool ok = SetupDiCreateDeviceInfoW(set, className, &cls, nullptr, nullptr, DICD_GENERATE_ID, &dev) &&
              SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(ids),
                                                static_cast<DWORD>((wcslen(ids) + 2) * sizeof(wchar_t))) &&
              SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
    if (!ok) {
        fail("creating device");
        SetupDiDestroyDeviceInfoList(set);
        return 4;
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        DWORD err = GetLastError();
        fprintf(stderr, "driver install failed: 0x%08lx\n", err);
        // Do not leave a phantom device with no driver behind.
        SetupDiCallClassInstaller(DIF_REMOVE, set, &dev);
        SetupDiDestroyDeviceInfoList(set);
        return 5;
    }
    SetupDiDestroyDeviceInfoList(set);

    CreateDirectoryW(kMarkerDir, nullptr);
    HANDLE h = CreateFileW(kMarker, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    printf("driver installed%s\n", reboot ? " (restart required)" : "");
    return 0;
}

int uninstall() {
    if (GetFileAttributesW(kMarker) == INVALID_FILE_ATTRIBUTES) {
        printf("driver was not installed by SideDroid; leaving it in place\n");
        return 0;
    }
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) {
        fail("SetupDiGetClassDevs");
        return 4;
    }
    SP_DEVINFO_DATA dev{sizeof(dev)};
    std::vector<std::wstring> infs;
    int removed = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        if (!hasHardwareId(set, dev)) continue;
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            wchar_t name[MAX_PATH]{};
            DWORD size = sizeof(name);
            if (RegQueryValueExW(key, L"InfPath", nullptr, nullptr, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS)
                infs.emplace_back(name);
            RegCloseKey(key);
        }
        if (SetupDiCallClassInstaller(DIF_REMOVE, set, &dev)) ++removed;
        else fail("removing device");
    }
    SetupDiDestroyDeviceInfoList(set);
    for (const auto& inf : infs) {
        // Only third-party packages are named oem*.inf: never touch anything else.
        if (_wcsnicmp(inf.c_str(), L"oem", 3) != 0) continue;
        if (!SetupUninstallOEMInfW(inf.c_str(), SUOI_FORCEDELETE, nullptr)) fail("removing driver package");
    }
    DeleteFileW(kMarker);
    RemoveDirectoryW(kMarkerDir);
    // This config exists only because we installed the driver.
    const std::wstring cfg = std::wstring(kConfigDir) + L"\\vdd_settings.xml";
    DeleteFileW(cfg.c_str());
    RemoveDirectoryW(kConfigDir);  // only succeeds when empty
    printf("removed %d device(s)\n", removed);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"status") {
        printf("device=%s installed_by_sidedroid=%s\n", deviceExists() ? "present" : "absent",
               GetFileAttributesW(kMarker) != INVALID_FILE_ATTRIBUTES ? "yes" : "no");
        return 0;
    }
    if (cmd == L"install") {
        std::wstring dir;
        if (argc > 2) {
            dir = argv[2];
        } else {
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            PathRemoveFileSpecW(self);
            dir = std::wstring(self) + L"\\driver";
        }
        return install(dir);
    }
    if (cmd == L"uninstall") return uninstall();
    fprintf(stderr, "usage: sidedroid-driver-tool install [package-dir] | uninstall | status\n");
    return 1;
}
