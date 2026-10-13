#include "platform/registration.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>

#include "config.h"

namespace {

using Microsoft::WRL::ComPtr;

LSTATUS SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           DWORD((value.size() + 1) * sizeof(wchar_t)));
}

// Per user Start menu shortcut, so Windows search finds the app.
std::wstring ShortcutPath() {
    PWSTR programs = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        path = std::wstring(programs) + L"\\" + kAppName + L".lnk";
    }
    CoTaskMemFree(programs);
    return path;
}

// Points the shortcut at exe. Its name is fixed, so a shortcut to a moved exe is replaced, not duplicated.
HRESULT UpdateShortcut(const wchar_t* exe) {
    const std::wstring path = ShortcutPath();
    if (path.empty()) return E_FAIL;
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    ComPtr<IPersistFile> file;
    if (SUCCEEDED(hr)) hr = link.As(&file);
    if (FAILED(hr)) return hr;

    wchar_t target[MAX_PATH * 4];
    if (SUCCEEDED(file->Load(path.c_str(), STGM_READ)) &&
        link->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH) == S_OK && _wcsicmp(target, exe) == 0) {
        return S_OK;
    }
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\'));
    hr = link->SetPath(exe);
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(dir.c_str());
    if (SUCCEEDED(hr)) hr = link->SetIconLocation(exe, 0);
    if (SUCCEEDED(hr)) hr = file->Save(path.c_str(), TRUE);
    return hr;
}

void RemoveOpenWith(const wchar_t* ext) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(),
                       kProgId);
}

// Removes every key a registration wrote, including extensions only an older version listed.
void RemoveKeys() {
    HKEY assoc;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(L"Software\\") + kAppId + L"\\Capabilities\\FileAssociations").c_str(),
                      0, KEY_QUERY_VALUE, &assoc) == ERROR_SUCCESS) {
        wchar_t ext[256];
        for (DWORD i = 0, size = ARRAYSIZE(ext);
             RegEnumValueW(assoc, i, ext, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_NO_MORE_ITEMS;
             ++i, size = ARRAYSIZE(ext)) {
            if (ext[0] == L'.') RemoveOpenWith(ext);
        }
        RegCloseKey(assoc);
    }
    for (const wchar_t* ext : kExtensions) RemoveOpenWith(ext);
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProgId).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\") + kAppId).c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppId);
}

}  // namespace

int Register() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (len == 0 || len == ARRAYSIZE(exe)) return 1;

    // Start clean, so nothing from an older version or another path is left behind.
    RemoveKeys();
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
    // Listed last, so Windows only sees the app once everything it points to is in place. RemoveKeys
    // deleted it first because value names keep their old casing, and Settings matches
    // registeredAppUser case-sensitively.
    status |= SetString(L"Software\\RegisteredApplications", kAppId, capsKey);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (status != ERROR_SUCCESS) return 1;

    // The shortcut and ShellExecute (for protocol links like ms-settings:) need COM.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    const HRESULT hr = UpdateShortcut(exe);
    if (SUCCEEDED(hr)) {
        ShellExecuteW(nullptr, nullptr, (std::wstring(L"ms-settings:defaultapps?registeredAppUser=") + kAppId).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}

int Unregister() {
    RemoveKeys();
    const std::wstring shortcut = ShortcutPath();
    if (!shortcut.empty()) DeleteFileW(shortcut.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return 0;
}
