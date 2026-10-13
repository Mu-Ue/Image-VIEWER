#include "platform/registration.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

#include "config.h"

namespace {

LSTATUS SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           DWORD((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

int Register() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (len == 0 || len == ARRAYSIZE(exe)) return 1;

    const std::wstring progKey = std::wstring(L"Software\\Classes\\") + kProgId;
    const std::wstring capsKey = std::wstring(L"Software\\") + kAppId + L"\\Capabilities";
    const std::wstring icon = L"\"" + std::wstring(exe) + L"\",0";
    LSTATUS status = SetString(progKey, nullptr, L"Image");
    status |= SetString(progKey + L"\\DefaultIcon", nullptr, icon);
    status |= SetString(progKey + L"\\shell\\open\\command", nullptr, L"\"" + std::wstring(exe) + L"\" \"%1\"");
    status |= SetString(capsKey, L"ApplicationName", kAppName);
    status |= SetString(capsKey, L"ApplicationDescription", L"A fast, minimal image viewer.");
    status |= SetString(capsKey, L"ApplicationIcon", icon);
    for (const wchar_t* ext : kExtensions) {
        status |= SetString(capsKey + L"\\FileAssociations", ext, kProgId);
        status |= RegSetKeyValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(),
                                  kProgId, REG_NONE, nullptr, 0);
    }
    // Listed last, so Windows only sees the app once everything it points to is in place. Deleted first
    // because value names keep their old casing, and Settings matches registeredAppUser case-sensitively.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppId);
    status |= SetString(L"Software\\RegisteredApplications", kAppId, capsKey);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (status != ERROR_SUCCESS) return 1;

    // ShellExecute needs COM for protocol links like ms-settings:.
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        ShellExecuteW(nullptr, nullptr, (std::wstring(L"ms-settings:defaultapps?registeredAppUser=") + kAppId).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }
    return 0;
}

int Unregister() {
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProgId).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\") + kAppId).c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppId);
    for (const wchar_t* ext : kExtensions) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(),
                           kProgId);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return 0;
}
