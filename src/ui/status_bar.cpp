#include "ui/status_bar.h"

#include <windowsx.h>

#include <span>

#include "config.h"
#include "ui/draw.h"
#include "ui/menu_bar.h"

namespace {

constexpr wchar_t kClassName[] = L"ImageVIEWERStatus";
constexpr wchar_t kChevrons[2][2] = {L"\xE76B", L"\xE76C"};  // ChevronLeft, ChevronRight in Segoe MDL2 Assets
constexpr UINT kButtonCmds[2] = {kCmdPrevious, kCmdNext};
constexpr UINT kMessageMs = 4000;  // how long a message stays

struct {
    HWND parent = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr, icons = nullptr;
    int height = 0, textH = 0, iconH = 0, pad = 0, gap = 0, buttonW = 0;
    RECT buttons[2]{};
    std::wstring fields[3] = {L"No file loaded"};  // resolution, size, zoom
    std::wstring message;  // shown instead of the fields while set
    int hot = -1, pressed = -1;  // button indices, -1 for none
    bool enabled = false;
} status;

// The fields, or the message instead while one is set.
std::span<const std::wstring> Shown() {
    return status.message.empty() ? std::span<const std::wstring>(status.fields) : std::span(&status.message, 1);
}

void Paint(HDC dc) {
    RECT client;
    GetClientRect(status.hwnd, &client);
    FillSolid(dc, client, kChrome);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, status.font);
    SetTextColor(dc, kChromeText);
    const int y = (status.height - status.textH) / 2;

    // Fields on the left, split by 1px lines a gap away from the text on each side, or the message instead.
    int x = status.gap;
    bool first = true;
    for (const std::wstring& field : Shown()) {
        if (field.empty()) continue;
        if (!first) {
            FillSolid(dc, {x, y, x + 1, y + status.textH}, kChromePressed);
            x += 1 + status.gap;
        }
        first = false;
        Text(dc, x, y, field.c_str());
        SIZE size;
        GetTextExtentPoint32W(dc, field.c_str(), int(field.size()), &size);
        x += size.cx + status.gap;
    }

    SelectObject(dc, status.icons);
    SetTextColor(dc, status.enabled ? kChromeText : kChromeTextDisabled);
    for (int i = 0; i < 2; ++i) {
        const RECT& r = status.buttons[i];
        if (status.enabled && (i == status.pressed || i == status.hot)) {
            FillRounded(dc, r, i == status.pressed ? kChromePressed : kChromeHover, status.height / 4);
        }
        SIZE size;
        GetTextExtentPoint32W(dc, kChevrons[i], 1, &size);
        Text(dc, r.left + (r.right - r.left - size.cx) / 2, r.top + (r.bottom - r.top - status.iconH) / 2,
             kChevrons[i]);
    }
}

// Centers the buttons in the bar.
void PlaceButtons(int width) {
    const int left = width / 2 - status.buttonW;
    for (int i = 0; i < 2; ++i) {
        const int x = left + i * status.buttonW;
        status.buttons[i] = {x, status.pad, x + status.buttonW, status.height - status.pad};
    }
}

// Repaints the bar and names its window after what it shows, which screen readers read out.
void Update() {
    std::wstring text;
    for (const std::wstring& field : Shown()) {
        if (field.empty()) continue;
        if (!text.empty()) text += L", ";
        text += field;
    }
    SetWindowTextW(status.hwnd, text.c_str());
    InvalidateRect(status.hwnd, nullptr, FALSE);
}

int HitTest(LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    for (int i = 0; i < 2; ++i) {
        if (PtInRect(&status.buttons[i], pt)) return i;
    }
    return -1;
}

LRESULT CALLBACK StatusProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            Paint(BeginPaint(hwnd, &ps));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            PlaceButtons(LOWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            const int hot = HitTest(lp);
            if (hot != status.hot) {
                status.hot = hot;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd};
                if (hot >= 0) TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            status.hot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            status.pressed = status.enabled ? HitTest(lp) : -1;
            if (status.pressed >= 0) {
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            const int pressed = status.pressed;
            if (pressed < 0) return 0;
            ReleaseCapture();  // clears pressed through WM_CAPTURECHANGED
            if (HitTest(lp) == pressed) SendMessageW(status.parent, WM_COMMAND, kButtonCmds[pressed], 0);
            return 0;
        }
        case WM_CAPTURECHANGED:
            status.pressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            KillTimer(hwnd, wp);
            status.message.clear();
            Update();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateStatusBar(HWND parent, HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = StatusProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return nullptr;

    status.parent = parent;
    LayoutStatusBar(GetDpiForWindow(parent));
    status.hwnd = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr,
                                  instance, nullptr);
    Update();
    return status.hwnd;
}

void LayoutStatusBar(UINT dpi) {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    if (status.font) DeleteObject(status.font);
    status.font = CreateFontIndirectW(&ncm.lfMenuFont);
    LOGFONTW icons{};
    icons.lfHeight = -MulDiv(10, dpi, 96);
    icons.lfWeight = FW_NORMAL;
    icons.lfCharSet = DEFAULT_CHARSET;
    icons.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(icons.lfFaceName, L"Segoe MDL2 Assets");
    if (status.icons) DeleteObject(status.icons);
    status.icons = CreateFontIndirectW(&icons);

    HDC dc = GetDC(nullptr);
    TEXTMETRICW tm;
    const HGDIOBJ old = SelectObject(dc, status.font);
    GetTextMetricsW(dc, &tm);
    status.textH = tm.tmHeight;
    SelectObject(dc, status.icons);
    GetTextMetricsW(dc, &tm);
    status.iconH = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);

    status.height = MenuBarHeight();
    status.pad = MulDiv(2, dpi, 96);
    status.gap = MulDiv(10, dpi, 96);
    status.buttonW = MulDiv(32, dpi, 96);
    if (status.hwnd) {
        RECT client;
        GetClientRect(status.hwnd, &client);
        PlaceButtons(client.right);
    }
}

int StatusBarHeight() {
    return status.height;
}

void SetStatus(std::wstring resolution, std::wstring size, long percent) {
    if (percent < 0) {
        resolution = L"No file loaded";
        size.clear();
    }
    std::wstring zoom = percent < 0 ? L"" : std::to_wstring(percent) + L"%";
    if (resolution == status.fields[0] && size == status.fields[1] && zoom == status.fields[2]) return;
    status.fields[0] = std::move(resolution);
    status.fields[1] = std::move(size);
    status.fields[2] = std::move(zoom);
    Update();
}

void ShowStatusMessage(std::wstring message) {
    status.message = std::move(message);
    SetTimer(status.hwnd, 1, kMessageMs, nullptr);  // restarts the wait if a message is already shown
    Update();
}

void EnableStepButtons(bool enable) {
    if (enable == status.enabled) return;
    status.enabled = enable;
    InvalidateRect(status.hwnd, nullptr, FALSE);
}
