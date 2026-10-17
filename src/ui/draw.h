#pragma once

#include <windows.h>

#include <cwchar>

#include "config.h"

// Drawing shared by the menu bar and the status bar, so both look the same.

// Colors of the bars and menus: Windows' dark gray, or the system colors while High Contrast is on.
struct Theme {
    bool highContrast = false;
    COLORREF chrome = kChrome, hover = kChromeHover, pressed = kChromePressed, line = kChromePressed;
    COLORREF text = kChromeText, hotText = kChromeText, disabledText = kChromeTextDisabled;
};

inline Theme theme;

// Reads the current theme; call again when the system colors or High Contrast change.
inline void LoadTheme() {
    HIGHCONTRASTW hc{sizeof(hc)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON)) {
        const COLORREF highlight = GetSysColor(COLOR_HIGHLIGHT);
        theme = {true, GetSysColor(COLOR_WINDOW), highlight, highlight, GetSysColor(COLOR_WINDOWTEXT),
                 GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_GRAYTEXT)};
    } else {
        theme = {};
    }
}

inline void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, color);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
}

inline void FillSolid(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, HBRUSH(GetStockObject(DC_BRUSH)));
}

// ExtTextOut rather than DrawText, which would load the complex-script shaper (TextShaping.dll).
inline void Text(HDC dc, int x, int y, const wchar_t* text) {
    ExtTextOutW(dc, x, y, 0, nullptr, text, UINT(wcslen(text)), nullptr);
}
