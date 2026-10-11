#pragma once

#include <windows.h>

#include <cwchar>

// Drawing shared by the menu bar and the status bar, so both look the same.

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
