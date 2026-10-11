#pragma once

#include <windows.h>

constexpr wchar_t kAppId[] = L"ImageVIEWER";
constexpr wchar_t kAppName[] = L"Image VIEWER";
constexpr wchar_t kProgId[] = L"ImageVIEWER.Image";
constexpr const wchar_t* kExtensions[] = {
    L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif", L".tif",
    L".tiff", L".ico", L".webp", L".heic", L".heif", L".avif", L".jxr", L".wdp",
};
constexpr wchar_t kFileFilter[] =
    L"Images\0*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.ico;"
    L"*.webp;*.heic;*.heif;*.avif;*.jxr;*.wdp\0";

constexpr UINT kMaxBitmapSize = 16384;  // Caps the decoded bitmap at 1 GB.
constexpr UINT64 kMaxSourcePixels = 1ull << 30;  // Bounds decode time for tiny files that claim huge sizes.
constexpr COLORREF kBackground = 0x000000;  // black
constexpr COLORREF kChrome = 0x202020;      // title bar and menu bar, Windows dark gray
constexpr COLORREF kChromeHover = 0x2D2D2D;
constexpr COLORREF kChromePressed = 0x383838;
constexpr COLORREF kChromeText = 0xE6E6E6;
constexpr COLORREF kChromeTextDisabled = 0x6E6E6E;
constexpr float kMaxZoom = 64.0f;
constexpr float kZoomStep = 1.25f;
