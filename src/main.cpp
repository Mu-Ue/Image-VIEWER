#include <windows.h>
#include <imm.h>

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

// Keeps AppInit DLLs, Winsock LSPs, global window hooks and legacy IMEs out of the viewer, ends it on
// any use of an invalid handle, and loads fonts only from the Windows fonts folder (the viewer never loads
// a font file itself). Not for the file dialog, which needs its shell extensions and IMEs. Dynamic code
// stays allowed: codecs can reach GPU drivers, which may generate code.
void HardenViewer() {
    PROCESS_MITIGATION_FONT_DISABLE_POLICY fonts{};
    fonts.DisableNonSystemFonts = 1;
    SetProcessMitigationPolicy(ProcessFontDisablePolicy, &fonts, sizeof(fonts));
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensions{};
    extensions.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extensions, sizeof(extensions));
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handles{};
    handles.RaiseExceptionOnInvalidHandleReference = 1;
    handles.HandleExceptionsPermanentlyEnabled = 1;
    SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &handles, sizeof(handles));
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
    HardenViewer();

    // The viewer has no text input, so never load the IME / text services stack (the file dialog runs
    // in its own process and keeps it).
    ImmDisableIME(DWORD(-1));

    if (!StartViewer(instance, std::move(path), showCmd)) return 1;

    // Handles all queued input before painting a pan, which waits for the frame timer.
    const HANDLE frame = FrameTimer();
    for (;;) {
        if (MsgWaitForMultipleObjectsEx(1, &frame, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0) {
            PresentFrame();
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return int(msg.wParam);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}
