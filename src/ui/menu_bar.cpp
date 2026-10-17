#include "ui/menu_bar.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>
#include <string>

#include "config.h"
#include "ui/draw.h"

namespace {

constexpr wchar_t kClassName[] = L"ImageVIEWERBar";

// A popup menu item; cmd 0 is a separator. key is the letter that picks it while its menu is open, marked
// as the mnemonic in the item's text, which Windows matches although the item is owner-drawn.
struct Item {
    UINT cmd;
    const wchar_t* label;
    wchar_t key;
    const wchar_t* shortcut;
};

constexpr Item kFileItems[] = {
    {kCmdOpen, L"Open…", 'O', L"Ctrl+O"},
    {kCmdCopy, L"Copy", 'C', L"Ctrl+C"},
    {kCmdPaste, L"Paste", 'P', L"Ctrl+V"},
    {kCmdSaveAs, L"Save As…", 'A', L"Ctrl+S"},
};

constexpr Item kViewItems[] = {
    {kCmdZoomIn, L"Zoom In", 'I', L"+"},
    {kCmdZoomOut, L"Zoom Out", 'O', L"-"},
    {kCmdFit, L"Fit to Window", 'F', L"0"},
    {kCmdActualSize, L"Actual Size", 'A', L"1"},
    {},
    {kCmdPrevious, L"Previous Image", 'P', L"Left"},
    {kCmdNext, L"Next Image", 'N', L"Right"},
    {},
    {kCmdFullscreen, L"Full Screen", 'U', L"F11"},
};

struct Menu {
    const wchar_t* label;
    const Item* items;
    int count;
    HMENU menu = nullptr;
    SIZE text{};
    RECT button{};
    int labelW = 0;  // widest item label, so every shortcut starts at the same x
};

struct {
    HWND parent = nullptr;
    HWND hwnd = nullptr;
    Menu menus[2] = {
        {L"File", kFileItems, int(std::size(kFileItems))},
        {L"View", kViewItems, int(std::size(kViewItems))},
    };
    HFONT font = nullptr;
    HBRUSH menuBrush = nullptr;
    int height = 0;
    int textH = 0, itemH = 0, separatorH = 0, padX = 0, gap = 0, inset = 0, shortcutW = 0;
    int hot = -1, open = -1;  // menu indices, -1 for none
    bool image = false, file = false;
} bar;

bool IsEnabled(UINT cmd) {
    switch (cmd) {
        case kCmdOpen: case kCmdPaste: case kCmdFullscreen: return true;
        case kCmdPrevious: case kCmdNext: return bar.file;
        default: return bar.image;
    }
}

// Rebuilds the popups, so Windows measures their owner-drawn items again for the current font and colors.
void BuildMenus() {
    if (bar.menuBrush) DeleteObject(bar.menuBrush);
    bar.menuBrush = CreateSolidBrush(theme.chrome);
    for (Menu& m : bar.menus) {
        if (m.menu) DestroyMenu(m.menu);
        m.menu = CreatePopupMenu();
        for (int i = 0; i < m.count; ++i) {
            // Owner-drawn, but given its text as well, which screen readers read out, with its key marked.
            const Item& item = m.items[i];
            std::wstring text = item.cmd ? std::wstring(item.label) + L'\t' + item.shortcut : L"";
            if (item.cmd) {
                const size_t upper = text.find(item.key);  // the capital, as in "Save &As", else as in "F&ull"
                text.insert(upper != text.npos ? upper : text.find(wchar_t(item.key - 'A' + 'a')), 1, L'&');
            }
            MENUITEMINFOW mii{sizeof(mii)};
            mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA | (item.cmd ? MIIM_STRING : 0);
            mii.fType = MFT_OWNERDRAW | (item.cmd ? 0 : MFT_SEPARATOR);
            mii.fState = item.cmd && !IsEnabled(item.cmd) ? MFS_GRAYED : 0;
            mii.wID = item.cmd;
            mii.dwItemData = ULONG_PTR(&item);
            mii.dwTypeData = text.data();
            InsertMenuItemW(m.menu, UINT(i), TRUE, &mii);
        }
        // No item is checkable, so drop the empty check-mark column on the left.
        MENUINFO mi{sizeof(mi), MIM_STYLE | MIM_BACKGROUND, MNS_NOCHECK};
        mi.hbrBack = bar.menuBrush;
        SetMenuInfo(m.menu, &mi);
    }
}

const Menu* MenuOf(const Item* item) {
    for (const Menu& m : bar.menus) {
        if (item >= m.items && item < m.items + m.count) return &m;
    }
    return nullptr;
}

void Paint(HDC dc) {
    RECT client;
    GetClientRect(bar.hwnd, &client);
    FillSolid(dc, client, theme.chrome);
    SelectObject(dc, bar.font);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < int(std::size(bar.menus)); ++i) {
        const Menu& m = bar.menus[i];
        const RECT& r = m.button;
        const bool lit = i == bar.open || i == bar.hot;
        if (lit) FillRounded(dc, r, i == bar.open ? theme.pressed : theme.hover, bar.height / 4);
        SetTextColor(dc, lit ? theme.hotText : theme.text);
        Text(dc, r.left + (r.right - r.left - m.text.cx) / 2, r.top + (r.bottom - r.top - m.text.cy) / 2, m.label);
    }
}

void DrawItem(const DRAWITEMSTRUCT& d) {
    const HDC dc = d.hDC;
    const RECT& r = d.rcItem;
    const int saved = SaveDC(dc);
    FillSolid(dc, r, theme.chrome);
    const Item* item = reinterpret_cast<const Item*>(d.itemData);
    if (!item->cmd) {
        const int y = (r.top + r.bottom) / 2;
        FillSolid(dc, {r.left + bar.inset, y, r.right - bar.inset, y + 1}, theme.line);
    } else {
        const bool selected = d.itemState & ODS_SELECTED;
        if (selected) {
            FillRounded(dc, {r.left + bar.inset, r.top, r.right - bar.inset, r.bottom}, theme.hover, bar.height / 4);
        }
        SelectObject(dc, bar.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, d.itemState & (ODS_GRAYED | ODS_DISABLED) ? theme.disabledText
                         : selected                               ? theme.hotText
                                                                  : theme.text);
        const int x = r.left + bar.padX, y = (r.top + r.bottom - bar.textH) / 2;
        Text(dc, x, y, item->label);
        Text(dc, x + MenuOf(item)->labelW + bar.gap, y, item->shortcut);
    }
    RestoreDC(dc, saved);
}

WNDPROC popupProc = nullptr;  // the menu window class's own procedure

// Rounds the popup and gives it a gray DWM border, then paints the classic 3D frame in the menu background
// so only that 1px border shows around it.
LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        const COLORREF border = theme.line;
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }
    if (msg == WM_NCPAINT) {
        RECT r;
        GetWindowRect(hwnd, &r);
        OffsetRect(&r, -r.left, -r.top);
        HDC dc = GetWindowDC(hwnd);
        FillSolid(dc, r, theme.chrome);
        ReleaseDC(hwnd, dc);
        return 0;
    }
    return CallWindowProcW(popupProc, hwnd, msg, wp, lp);
}

// Owner-drawn menus are not themed, so their window gets the classic 3D frame. Catch each popup as it is
// created to round its corners and give it a thin gray border, like the system's own menus on Windows 11.
LRESULT CALLBACK CbtHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        const HWND hwnd = HWND(wp);
        wchar_t name[8];
        if (GetClassNameW(hwnd, name, int(std::size(name))) && !wcscmp(name, L"#32768")) {
            popupProc = WNDPROC(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, LONG_PTR(PopupProc)));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

int HitTest(LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    for (int i = 0; i < int(std::size(bar.menus)); ++i) {
        if (PtInRect(&bar.menus[i].button, pt)) return i;
    }
    return -1;
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
        case WM_NCHITTEST: {  // let the space beside the buttons through to the parent, which drags the window
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            return HitTest(MAKELPARAM(pt.x, pt.y)) < 0 ? HTTRANSPARENT : HTCLIENT;
        }
        case WM_MOUSEMOVE: {
            const int hot = HitTest(lp);
            if (hot != bar.hot) {
                bar.hot = hot;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd};
                if (hot >= 0) TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            bar.hot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const int hit = HitTest(lp);
            if (hit >= 0) ShowMenu(MenuIndex(hit), false);
            return 0;
        }
        case WM_MEASUREITEM: {
            auto& mi = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            const Item* item = reinterpret_cast<const Item*>(mi.itemData);
            mi.itemWidth = UINT(2 * bar.padX + MenuOf(item)->labelW + bar.gap + bar.shortcutW);
            mi.itemHeight = UINT(item->cmd ? bar.itemH : bar.separatorH);
            return TRUE;
        }
        case WM_DRAWITEM:
            DrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_COMMAND:
            return SendMessageW(bar.parent, msg, wp, lp);
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

    HDC dc = GetDC(nullptr);
    const HGDIOBJ old = SelectObject(dc, bar.font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    bar.textH = tm.tmHeight;
    bar.shortcutW = 0;
    for (Menu& m : bar.menus) {
        GetTextExtentPoint32W(dc, m.label, int(wcslen(m.label)), &m.text);
        m.labelW = 0;
        for (int i = 0; i < m.count; ++i) {
            const Item& item = m.items[i];
            if (!item.cmd) continue;
            SIZE size;
            GetTextExtentPoint32W(dc, item.label, int(wcslen(item.label)), &size);
            m.labelW = std::max(m.labelW, int(size.cx));
            GetTextExtentPoint32W(dc, item.shortcut, int(wcslen(item.shortcut)), &size);
            bar.shortcutW = std::max(bar.shortcutW, int(size.cx));
        }
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);

    const int pad = MulDiv(2, dpi, 96);
    bar.height = MulDiv(24, dpi, 96);
    bar.itemH = bar.textH + MulDiv(12, dpi, 96);
    bar.separatorH = MulDiv(9, dpi, 96);
    bar.inset = MulDiv(3, dpi, 96);
    bar.padX = bar.inset + MulDiv(12, dpi, 96);
    bar.gap = MulDiv(40, dpi, 96);
    // Flush left so the first button lines up with its popup, which opens at the window edge.
    int x = 0;
    for (Menu& m : bar.menus) {
        m.button = {x, pad, x + m.text.cx + MulDiv(20, dpi, 96), bar.height - pad};
        x = m.button.right;
    }
    BuildMenus();
}

int MenuBarHeight() {
    return bar.height;
}

void ShowMenu(MenuIndex index, bool keyboard) {
    bar.open = index;
    RedrawWindow(bar.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    // Open below the whole bar, not over its bottom padding.
    const Menu& m = bar.menus[index];
    RECT r = {m.button.left, 0, m.button.right, bar.height};
    MapWindowPoints(bar.hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    TPMPARAMS params{sizeof(params), r};
    if (keyboard) PostMessageW(bar.hwnd, WM_KEYDOWN, VK_DOWN, 0);  // select the first item
    const HHOOK hook = SetWindowsHookExW(WH_CBT, CbtHook, nullptr, GetCurrentThreadId());
    TrackPopupMenuEx(m.menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, r.left, r.bottom, bar.hwnd, &params);
    if (hook) UnhookWindowsHookEx(hook);
    // The click that closed the menu must not reopen it.
    MSG msg;
    PeekMessageW(&msg, bar.hwnd, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE);
    bar.open = -1;
    bar.hot = -1;
    InvalidateRect(bar.hwnd, nullptr, FALSE);
}

void EnableImageCommands(bool image, bool file) {
    bar.image = image;
    bar.file = file;
    for (const Menu& m : bar.menus) {
        for (int i = 0; i < m.count; ++i) {
            const UINT cmd = m.items[i].cmd;
            if (cmd) EnableMenuItem(m.menu, cmd, MF_BYCOMMAND | (IsEnabled(cmd) ? MF_ENABLED : MF_GRAYED));
        }
    }
}
