#include "platform/file_dialog.h"

#include <objbase.h>
#include <commdlg.h>
#include <shellapi.h>

#include "config.h"

int DialogMain() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 5 || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    const bool save = wcscmp(argv[2], L"save") == 0;
    const HWND owner = reinterpret_cast<HWND>(UINT_PTR(_wcstoui64(argv[3], nullptr, 10)));

    wchar_t buffer[32768] = L"";
    wcsncpy_s(buffer, argv[4], _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = IsWindow(owner) ? owner : nullptr;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = ARRAYSIZE(buffer);
    if (save) {
        ofn.lpstrFilter = L"PNG\0*.png\0JPEG\0*.jpg;*.jpeg\0";
        ofn.lpstrDefExt = L"png";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    } else {
        ofn.lpstrFilter = kFileFilter;
        ofn.lpstrTitle = kAppName;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    }
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return 1;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, DWORD(wcslen(buffer) * sizeof(wchar_t)), &written, nullptr);
    return 0;
}

std::wstring RunFileDialog(HWND owner, const wchar_t* kind, const std::wstring& name) {
    wchar_t exe[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (len == 0 || len == ARRAYSIZE(exe)) return {};
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --dialog " + kind + L" " +
                       std::to_wstring(UINT_PTR(owner)) + L" \"" + name + L"\"";

    // The pipe holds the longest path, so the helper never blocks writing it.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 65536 + 4096)) return {};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(write);
    if (!started) {
        CloseHandle(read);
        return {};
    }
    CloseHandle(pi.hThread);
    AllowSetForegroundWindow(pi.dwProcessId);

    // Modal like an in-process dialog, but keep painting while it is open.
    EnableWindow(owner, FALSE);
    bool quit = false;
    while (!quit && MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(int(msg.wParam));
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    CloseHandle(pi.hProcess);

    std::wstring path;
    wchar_t chunk[4096];
    DWORD n = 0;
    while (!quit && ReadFile(read, chunk, sizeof(chunk), &n, nullptr) && n) path.append(chunk, n / sizeof(wchar_t));
    CloseHandle(read);
    return path;
}
