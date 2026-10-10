#include <windows.h>
#include <objbase.h>

#include <string>

#include "platform/file_dialog.h"
#include "platform/registration.h"
#include "ui/viewer.h"

namespace {

// Loads DLLs only from System32 and never from network shares or low-integrity locations,
// so a DLL planted next to the exe or in the current folder is never picked up.
void HardenProcess() {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy{};
    policy.NoRemoteImages = 1;
    policy.NoLowMandatoryLabelImages = 1;
    policy.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &policy, sizeof(policy));
}

std::wstring Unquote(const wchar_t* s) {
    std::wstring arg(s ? s : L"");
    const auto first = arg.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    arg = arg.substr(first, arg.find_last_not_of(L" \t") - first + 1);
    if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
    return arg;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int showCmd) {
    HardenProcess();
    if (wcsncmp(cmdLine, L"--dialog ", 9) == 0) return DialogMain();
    std::wstring path = Unquote(cmdLine);
    if (path == L"--register") return Register();
    if (path == L"--unregister") return Unregister();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    if (!StartViewer(instance, std::move(path), showCmd)) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
