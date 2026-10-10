#include "ui/menu_bar.h"

#include <windowsx.h>

#include "config.h"

namespace {

constexpr wchar_t kClassName[] = L"ImageViewerBar";

struct {
    HWND parent = nullptr;
    HWND hwnd = nullptr;
    HMENU fileMenu = nullptr;
    HFONT font = nullptr;
    int height = 0;
    RECT fileButton{};
    bool fileHot = false, menuOpen = false;
} bar;

void Paint(HDC dc) {
    RECT client;
    GetClientRect(bar.hwnd, &client);
    SetDCBrushColor(dc, kChrome);
    FillRect(dc, &client, HBRUSH(GetStockObject(DC_BRUSH)));
    const RECT& r = bar.fileButton;
    if (bar.menuOpen || bar.fileHot) {
        const COLORREF color = bar.menuOpen ? kChromePressed : kChromeHover;
        SetDCBrushColor(dc, color);
        SetDCPenColor(dc, color);
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        const int radius = bar.height / 4;
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    }
    SelectObject(dc, bar.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kChromeText);
    RECT text = r;
    DrawTextW(dc, L"File", 4, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            Paint(BeginPaint(hwnd, &ps));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            const bool hot = PtInRect(&bar.fileButton, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot != bar.fileHot) {
                bar.fileHot = hot;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd};
                if (hot) TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            bar.fileHot = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            if (PtInRect(&bar.fileButton, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) ShowFileMenu(false);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateMenuBar(HWND parent, HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = BarProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return nullptr;

    bar.fileMenu = CreatePopupMenu();
    AppendMenuW(bar.fileMenu, MF_STRING | MF_GRAYED, kCmdNew, L"&New\tCtrl+N");
    AppendMenuW(bar.fileMenu, MF_STRING, kCmdOpen, L"&Open…\tCtrl+O");
    AppendMenuW(bar.fileMenu, MF_STRING | MF_GRAYED, kCmdSaveAs, L"Save &As…\tCtrl+S");
    // No item is checkable, so drop the empty check-mark column on the left.
    MENUINFO mi{sizeof(mi), MIM_STYLE, MNS_NOCHECK};
    SetMenuInfo(bar.fileMenu, &mi);

    bar.parent = parent;
    LayoutMenuBar(GetDpiForWindow(parent));
    bar.hwnd = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr,
                               instance, nullptr);
    return bar.hwnd;
}

void LayoutMenuBar(UINT dpi) {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    if (bar.font) DeleteObject(bar.font);
    bar.font = CreateFontIndirectW(&ncm.lfMenuFont);

    SIZE text{};
    HDC dc = GetDC(nullptr);
    const HGDIOBJ old = SelectObject(dc, bar.font);
    GetTextExtentPoint32W(dc, L"File", 4, &text);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);

    const int pad = MulDiv(4, dpi, 96);
    bar.height = MulDiv(32, dpi, 96);
    // Flush left so the button lines up with its popup, which opens at the window edge.
    bar.fileButton = {0, pad, text.cx + MulDiv(20, dpi, 96), bar.height - pad};
}

int MenuBarHeight() {
    return bar.height;
}

void ShowFileMenu(bool keyboard) {
    bar.menuOpen = true;
    RedrawWindow(bar.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    // Open below the whole bar, not over its bottom padding.
    RECT r = {bar.fileButton.left, 0, bar.fileButton.right, bar.height};
    MapWindowPoints(bar.hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    TPMPARAMS params{sizeof(params), r};
    if (keyboard) PostMessageW(bar.parent, WM_KEYDOWN, VK_DOWN, 0);  // select the first item
    TrackPopupMenuEx(bar.fileMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, r.left, r.bottom, bar.parent,
                     &params);
    // The click that closed the menu must not reopen it.
    MSG msg;
    PeekMessageW(&msg, bar.hwnd, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE);
    bar.menuOpen = false;
    bar.fileHot = false;
    InvalidateRect(bar.hwnd, nullptr, FALSE);
}

void EnableImageCommands(bool enabled) {
    const UINT state = MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(bar.fileMenu, kCmdNew, state);
    EnableMenuItem(bar.fileMenu, kCmdSaveAs, state);
}
