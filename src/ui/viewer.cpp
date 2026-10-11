#include "ui/viewer.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <thread>

#include "config.h"
#include "imaging/decoder.h"
#include "imaging/encoder.h"
#include "platform/clipboard.h"
#include "platform/file_dialog.h"
#include "ui/menu_bar.h"
#include "ui/scale.h"

namespace {

struct Viewer {
    HWND hwnd = nullptr;
    HWND bar = nullptr;
    int barH = 0;
    Source source;  // of the current image, re-read when saving
    std::wstring name;

    Image image;
    HDC imageDC = nullptr;  // memory DC holding image.bitmap
    HGDIOBJ emptyBitmap = nullptr;  // imageDC's original bitmap, selected while no image is shown
    HBRUSH background = nullptr;

    float imageW = 0, imageH = 0;  // upright size in pixels
    float clientW = 0, clientH = 0;
    float zoom = 1, offsetX = 0, offsetY = 0;  // offset = image top-left in client pixels
    bool fit = true;
    bool dragging = false;
    POINT dragFrom{};
    long shownPercent = -1;
} g;

// Draws the image area where update (in image area coordinates) needs it.
void Render(HDC dc, HRGN update) {
    const float z = g.zoom;
    const RECT client{0, 0, LONG(g.clientW), LONG(g.clientH)};
    if (g.image.bitmap) {
        // Each screen pixel is the exact average of its image area, computed only where painted, one
        // rectangle at a time so the L-shaped strips a diagonal pan uncovers stay small. The drawn size
        // depends on the zoom alone, so a pan never changes which image area a pixel covers.
        const int x = int(std::round(g.offsetX)), y = int(std::round(g.offsetY));
        const int w = int(std::lround(double(g.image.width) * z));
        const int h = int(std::lround(double(g.image.height) * z));
        union {
            RGNDATA data;
            BYTE bytes[sizeof(RGNDATAHEADER) + 8 * sizeof(RECT)];
        } region;
        RECT box;
        const RECT* rects = &box;
        DWORD count = 1;
        if (GetRegionData(update, sizeof(region), &region.data)) {
            rects = reinterpret_cast<const RECT*>(region.data.Buffer);
            count = region.data.rdh.nCount;
        } else {  // too many rectangles: draw their bounds
            GetClipBox(dc, &box);
        }
        bool drawn = true;
        for (DWORD i = 0; i < count && drawn; ++i) {
            RECT r;
            if (IntersectRect(&r, &rects[i], &client)) drawn = DrawScaled(dc, g.imageDC, x, y, w, h, r);
        }
        if (drawn) {
            ExcludeClipRect(dc, x, y, x + w, y + h);
            FillRect(dc, &client, g.background);
            return;
        }
    }

    // Only stretch the part of the image that is on screen.
    const int sx0 = std::max(0, int(std::floor(-g.offsetX / z)));
    const int sy0 = std::max(0, int(std::floor(-g.offsetY / z)));
    const int sx1 = std::min(int(g.image.width), int(std::ceil((g.clientW - g.offsetX) / z)));
    const int sy1 = std::min(int(g.image.height), int(std::ceil((g.clientH - g.offsetY) / z)));
    if (sx1 > sx0 && sy1 > sy0) {
        const float ox = std::round(g.offsetX), oy = std::round(g.offsetY);
        const int dx0 = int(std::lround(ox + sx0 * z)), dy0 = int(std::lround(oy + sy0 * z));
        const int dx1 = int(std::lround(ox + sx1 * z)), dy1 = int(std::lround(oy + sy1 * z));
        SetStretchBltMode(dc, z >= 3.0f ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, dx0, dy0, dx1 - dx0, dy1 - dy0, g.imageDC, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
        ExcludeClipRect(dc, dx0, dy0, dx1, dy1);
    }
    FillRect(dc, &client, g.background);
}

float FitZoom() {
    if (g.imageW <= 0 || g.clientW <= 0 || g.clientH <= 0) return 1.0f;
    return std::min({1.0f, g.clientW / g.imageW, g.clientH / g.imageH});
}

// Centers the image on any axis where it fits, otherwise keeps the window covered.
void ClampOffset() {
    const float w = g.imageW * g.zoom, h = g.imageH * g.zoom;
    g.offsetX = w <= g.clientW ? (g.clientW - w) / 2 : std::clamp(g.offsetX, g.clientW - w, 0.0f);
    g.offsetY = h <= g.clientH ? (g.clientH - h) / 2 : std::clamp(g.offsetY, g.clientH - h, 0.0f);
}

void UpdateTitle() {
    if (!g.image.bitmap) return;
    const long percent = std::lround(g.zoom * 100);
    if (percent == g.shownPercent) return;
    g.shownPercent = percent;
    SetWindowTextW(g.hwnd, (g.name + L"  ·  " + std::to_wstring(percent) + L"%").c_str());
}

void ZoomAt(float zoom, float x, float y) {
    if (!g.image.bitmap) return;
    const float fitZoom = FitZoom();
    zoom = std::clamp(zoom, fitZoom, std::max(kMaxZoom, fitZoom));
    if (zoom == g.zoom) return;
    g.offsetX = x - (x - g.offsetX) * (zoom / g.zoom);
    g.offsetY = y - (y - g.offsetY) * (zoom / g.zoom);
    g.zoom = zoom;
    g.fit = zoom == fitZoom;
    ClampOffset();
    UpdateTitle();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Moves the image and paints it right away. Drawn pixels are exact relative to the image's top left, so
// they are scrolled and only the uncovered strips are computed.
void Pan(float dx, float dy) {
    const float x0 = std::round(g.offsetX), y0 = std::round(g.offsetY);
    g.offsetX += dx;
    g.offsetY += dy;
    ClampOffset();
    const int sx = int(std::round(g.offsetX) - x0), sy = int(std::round(g.offsetY) - y0);
    if (sx == 0 && sy == 0) return;
    const RECT area{0, g.barH, LONG(g.clientW), g.barH + LONG(g.clientH)};
    ScrollWindowEx(g.hwnd, sx, sy, &area, &area, nullptr, nullptr, SW_INVALIDATE);
    UpdateWindow(g.hwnd);
}

void OnSize(float width, float height) {
    if (width <= 0 || height <= 0) return;  // minimized
    const float dx = (width - g.clientW) / 2, dy = (height - g.clientH) / 2;
    g.clientW = width;
    g.clientH = height;
    if (g.fit || g.zoom < FitZoom()) {
        g.zoom = FitZoom();
        g.fit = true;
    } else {
        g.offsetX += dx;  // keep the view centered on the same spot
        g.offsetY += dy;
    }
    ClampOffset();
    UpdateTitle();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Sizes the window to the image (capped to the work area), or a default size when empty,
// and centers it on the active monitor.
void PlaceWindow() {
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const float workW = float(work.right - work.left), workH = float(work.bottom - work.top);

    const UINT dpi = GetDpiForWindow(g.hwnd);
    RECT rc{0, 0, MulDiv(960, dpi, 96), MulDiv(640, dpi, 96)};
    if (g.image.bitmap) {
        const float scale = std::min({1.0f, workW * 0.85f / g.imageW, workH * 0.85f / g.imageH});
        rc.right = std::max(LONG(std::lround(g.imageW * scale)), LONG(MulDiv(480, dpi, 96)));
        rc.bottom = std::max(LONG(std::lround(g.imageH * scale)), LONG(MulDiv(320, dpi, 96)));
    }
    rc.bottom += g.barH;
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(g.hwnd, nullptr, work.left + (int(workW) - w) / 2, work.top + (int(workH) - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ShowError(HRESULT hr, const std::wstring& name) {
    std::wstring message = L"Can't open \"" + name + L"\".";
    if (hr == WINCODEC_ERR_UNKNOWNIMAGEFORMAT) {
        message += L"\n\nIt isn't a supported image.";
    } else if (hr == WINCODEC_ERR_IMAGESIZEOUTOFRANGE) {
        message += L"\n\nIt's too large to open.";
    } else if (hr == WINCODEC_ERR_COMPONENTNOTFOUND) {
        message += L"\n\nThis format isn't supported. Codecs for HEIC/AVIF are available from the Microsoft Store.";
    } else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        message += L"\n\nThe file doesn't exist.";
    }
    MessageBoxW(g.hwnd, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

// Shows a decoded image, replacing the current one.
void SetImage(const Image& image, std::wstring name) {
    SelectObject(g.imageDC, image.bitmap);
    if (g.image.bitmap) DeleteObject(g.image.bitmap);
    g.image = image;
    g.name = std::move(name);
    g.imageW = float(image.width);
    g.imageH = float(image.height);
    g.zoom = FitZoom();
    g.fit = true;
    g.shownPercent = -1;
    ClampOffset();
    UpdateTitle();
    EnableImageCommands(true);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Closes the current image, leaving the empty window.
void CloseImage() {
    if (!g.image.bitmap) return;
    SelectObject(g.imageDC, g.emptyBitmap);
    DeleteObject(g.image.bitmap);
    StopScaleThreads();
    g.image = {};
    g.source = {};
    g.name.clear();
    g.imageW = g.imageH = 0;
    g.zoom = 1;
    g.fit = true;
    g.shownPercent = -1;
    ClampOffset();
    SetWindowTextW(g.hwnd, kAppName);
    EnableImageCommands(false);
    // Return the heap memory freed by decoding, so New leaves the process as it was at launch.
    HEAP_OPTIMIZE_RESOURCES_INFORMATION info{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
    HeapSetInformation(nullptr, HeapOptimizeResources, &info, sizeof(info));
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Runs work on a new thread with its own COM apartment, setting hr to its result. The UI thread never
// starts COM, so when the thread ends COM unloads WIC and its codecs instead of keeping them in memory.
template <class Work>
std::thread ComThread(HRESULT& hr, Work work) {
    return std::thread([&hr, work] {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            hr = work();
            CoUninitialize();
        }
    });
}

// Decodes and shows an image, keeping the current one if that fails. Returns whether it is shown.
bool Load(Source src, std::wstring name, bool showError = true) {
    Image image;
    HRESULT hr;
    ComThread(hr, [&] { return DecodeImage(src, image); }).join();
    if (FAILED(hr)) {
        if (showError) ShowError(hr, name);
        return false;
    }
    // A pasted copy is no longer needed once the bitmap holds every pixel of it.
    if (src.stream && image.exact) src = {.bitmap = image.bitmap};
    g.source = std::move(src);
    SetImage(image, std::move(name));
    return true;
}

std::wstring FileName(const std::wstring& path) {
    return path.substr(path.find_last_of(L"\\/") + 1);
}

void Open() {
    std::wstring path = RunFileDialog(g.hwnd, L"open", {});
    if (path.empty()) return;
    std::wstring name = FileName(path);
    Load({std::move(path)}, std::move(name));
}

bool IsImageName(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    for (const wchar_t* ext : kExtensions) {
        if (CompareStringOrdinal(dot, -1, ext, -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}

// File names are not case-sensitive.
bool SamePath(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL;
}

// Orders names like Explorer ("2" before "10"), breaking ties so no two names compare equal.
int CompareNames(const wchar_t* a, const wchar_t* b) {
    const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a, -1, b, -1,
                                  nullptr, nullptr, 0);
    return r != CSTR_EQUAL ? r - CSTR_EQUAL : CompareStringOrdinal(a, -1, b, -1, FALSE) - CSTR_EQUAL;
}

// Sets around[0] and around[1] to the images before and after path in its folder, wrapping around at the ends.
// Both are empty if the folder holds no other image. Lists the folder each time, so it is never out of date.
void Neighbors(const std::wstring& path, std::wstring (&around)[2]) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
    const wchar_t* name = path.c_str() + dir.size();
    // Closest before and after, and the last and first for wrapping around.
    std::wstring before, after, last, first;
    WIN32_FIND_DATAW fd;
    const HANDLE find = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                         nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* f = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsImageName(f) || SamePath(f, name)) continue;
            const int c = CompareNames(f, name);
            if (c < 0) {
                if (before.empty() || CompareNames(f, before.c_str()) > 0) before = f;
                if (first.empty() || CompareNames(f, first.c_str()) < 0) first = f;
            } else if (c > 0) {
                if (after.empty() || CompareNames(f, after.c_str()) < 0) after = f;
                if (last.empty() || CompareNames(f, last.c_str()) > 0) last = f;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (before.empty()) before = last;
    if (after.empty()) after = first;
    around[0] = before.empty() ? L"" : dir + before;
    around[1] = after.empty() ? L"" : dir + after;
}

// Shows the previous (step < 0) or next image in the folder, silently skipping files that can't be opened.
void Step(int step) {
    if (g.source.path.empty()) return;
    const std::wstring start = g.source.path;
    std::wstring from = start, first;
    for (;;) {
        std::wstring around[2];
        Neighbors(from, around);
        std::wstring& to = around[step > 0];
        if (to.empty() || SamePath(to, start) || to == first) return;  // nothing else, or every other file failed
        if (first.empty()) first = to;
        std::wstring name = FileName(to);
        if (Load({to}, std::move(name), false)) return;
        from = std::move(to);
    }
}

void SaveAs() {
    if (!g.image.bitmap) return;
    const std::wstring path = RunFileDialog(g.hwnd, L"save", g.name.substr(0, g.name.find_last_of(L'.')));
    if (path.empty()) return;
    HRESULT hr;
    ComThread(hr, [&] { return SaveImage(g.source, path); }).join();
    if (FAILED(hr)) {
        const std::wstring message = L"Can't save \"" + path + L"\".";
        MessageBoxW(g.hwnd, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
    }
}

void Paste() {
    Source src;
    const HRESULT hr = CopyClipboardImage(g.hwnd, src.stream);
    if (hr == S_OK) {
        Load(std::move(src), L"Pasted image");
    } else if (FAILED(hr)) {
        ShowError(hr, L"Pasted image");
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.bar = CreateMenuBar(hwnd, reinterpret_cast<const CREATESTRUCTW*>(lp)->hInstance);
            g.barH = MenuBarHeight();
            return g.bar ? 0 : -1;
        case WM_PAINT: {
            const HRGN update = CreateRectRgn(0, 0, 0, 0);
            GetUpdateRgn(hwnd, update, FALSE);
            OffsetRgn(update, 0, -g.barH);
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(hwnd, &ps);
            SetViewportOrgEx(dc, 0, g.barH, nullptr);  // the image area starts below the bar
            Render(dc, update);
            EndPaint(hwnd, &ps);
            DeleteObject(update);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            MoveWindow(g.bar, 0, 0, LOWORD(lp), g.barH, TRUE);
            OnSize(float(LOWORD(lp)), float(HIWORD(lp) - g.barH));
            return 0;
        case WM_MOUSEWHEEL: {  // also delivered for touchpad pinch
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            pt.y -= g.barH;
            const float notches = GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA);
            ZoomAt(g.zoom * std::pow(kZoomStep, notches), float(pt.x), float(pt.y));
            return 0;
        }
        case WM_LBUTTONDOWN:
            g.dragging = true;
            g.dragFrom = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (g.dragging) {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                Pan(float(pt.x - g.dragFrom.x), float(pt.y - g.dragFrom.y));
                g.dragFrom = pt;
            }
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            g.dragging = false;
            return 0;
        case WM_LBUTTONDBLCLK: {  // toggle fit <-> 100% (2x for images smaller than the window)
            const float fitZoom = FitZoom();
            ZoomAt(g.fit ? (fitZoom < 1.0f ? 1.0f : 2.0f) : fitZoom,
                   float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp) - g.barH));
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdNew: CloseImage(); break;
                case kCmdOpen: Open(); break;
                case kCmdSaveAs: SaveAs(); break;
            }
            return 0;
        case WM_SYSCHAR:
            if (wp == 'f' || wp == 'F') {
                ShowFileMenu(true);
                return 0;
            }
            break;
        case WM_KEYDOWN: {
            if (GetKeyState(VK_CONTROL) < 0) {
                switch (wp) {
                    case 'N': CloseImage(); break;
                    case 'O': Open(); break;
                    case 'S': SaveAs(); break;
                    case 'V': Paste(); break;
                }
                return 0;
            }
            const float cx = g.clientW / 2, cy = g.clientH / 2;
            switch (wp) {
                case VK_OEM_PLUS: case VK_ADD: ZoomAt(g.zoom * kZoomStep, cx, cy); break;
                case VK_OEM_MINUS: case VK_SUBTRACT: ZoomAt(g.zoom / kZoomStep, cx, cy); break;
                case '0': case VK_NUMPAD0: ZoomAt(FitZoom(), cx, cy); break;
                case '1': case VK_NUMPAD1: ZoomAt(1.0f, cx, cy); break;
                case VK_LEFT: Step(-1); break;
                case VK_RIGHT: Step(1); break;
                case VK_ESCAPE: DestroyWindow(hwnd); break;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            LayoutMenuBar(HIWORD(wp));
            g.barH = MenuBarHeight();
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Dark popup menus. uxtheme's SetPreferredAppMode (ordinal 135, Windows 10 1903+) is undocumented,
// so it is looked up at run time and skipped if missing.
void EnableDarkMenus() {
    using SetPreferredAppMode = int(WINAPI*)(int);
    constexpr int kForceDark = 2;
    if (HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        const auto set = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        if (set) set(kForceDark);
    }
}

HWND CreateMainWindow(HINSTANCE instance) {
    EnableDarkMenus();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g.background = CreateSolidBrush(kBackground);
    wc.lpszClassName = kAppId;
    if (!RegisterClassExW(&wc)) return nullptr;

    HWND hwnd = CreateWindowExW(0, kAppId, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, nullptr);
    if (hwnd) {
        // Dark title bar that blends into the menu bar (caption color is Windows 11 only).
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF caption = kChrome;
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    }
    return hwnd;
}

}  // namespace

bool StartViewer(HINSTANCE instance, std::wstring path, int showCmd) {
    g.source.path = std::move(path);
    g.name = g.source.path.substr(g.source.path.find_last_of(L"\\/") + 1);

    // Decode on a worker thread while the window is set up.
    Image image;
    HRESULT decodeHr = S_OK;
    std::thread decoder;
    if (!g.source.path.empty()) decoder = ComThread(decodeHr, [&] { return DecodeImage(g.source, image); });

    g.hwnd = CreateMainWindow(instance);
    if (decoder.joinable()) decoder.join();
    if (FAILED(decodeHr)) {
        ShowError(decodeHr, g.name);
        return false;
    }
    if (!g.hwnd) return false;

    g.imageDC = CreateCompatibleDC(nullptr);
    if (!g.imageDC) return false;
    g.emptyBitmap = GetCurrentObject(g.imageDC, OBJ_BITMAP);
    if (image.bitmap) SetImage(image, g.name);

    PlaceWindow();
    ShowWindow(g.hwnd, showCmd);
    UpdateWindow(g.hwnd);
    return true;
}
