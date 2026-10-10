#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kAppId[] = L"ImageViewer";
constexpr wchar_t kAppName[] = L"Image Viewer";
constexpr wchar_t kProgId[] = L"ImageViewer.Image";
constexpr const wchar_t* kExtensions[] = {
    L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif", L".tif",
    L".tiff", L".ico", L".webp", L".heic", L".heif", L".avif", L".jxr", L".wdp",
};
constexpr wchar_t kFileFilter[] =
    L"Images\0*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.ico;"
    L"*.webp;*.heic;*.heif;*.avif;*.jxr;*.wdp\0All files\0*.*\0";

constexpr UINT kMaxBitmapSize = 16384;  // Caps the decoded bitmap at 1 GB.
constexpr COLORREF kBackground = 0x202020;  // neutral gray
constexpr float kMaxZoom = 64.0f;
constexpr float kZoomStep = 1.25f;

struct Image {
    HBITMAP bitmap = nullptr;  // 32bpp top-down DIB section, already composited over kBackground
    UINT width = 0;
    UINT height = 0;
};

// Reads the EXIF orientation tag, if any.
int ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    for (const wchar_t* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
            const int orientation = value.vt == VT_UI2 ? value.uiVal : 1;
            PropVariantClear(&value);
            if (orientation >= 1 && orientation <= 8) return orientation;
        }
    }
    return 1;
}

// Bits per pixel of a frame's native pixel format, or 0 if unknown.
UINT BitsPerPixel(IWICImagingFactory* wic, IWICBitmapSource* source) {
    WICPixelFormatGUID format;
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo> formatInfo;
    UINT bpp = 0;
    if (SUCCEEDED(source->GetPixelFormat(&format)) && SUCCEEDED(wic->CreateComponentInfo(format, &info)) &&
        SUCCEEDED(info.As(&formatInfo)) && SUCCEEDED(formatInfo->GetBitsPerPixel(&bpp))) {
        return bpp;
    }
    return 0;
}

// Icons hold several sizes and frame 0 is often the smallest, so pick the largest, deepest one.
HRESULT GetFrame(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, ComPtr<IWICBitmapFrameDecode>& out) {
    HRESULT hr = decoder->GetFrame(0, &out);
    GUID container;
    UINT count = 0;
    if (FAILED(hr) || FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatIco ||
        FAILED(decoder->GetFrameCount(&count))) {
        return hr;
    }
    UINT64 best = 0;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT w = 0, h = 0;
        if (FAILED(decoder->GetFrame(i, &frame)) || FAILED(frame->GetSize(&w, &h))) continue;
        const UINT64 score = (UINT64(w) * h << 8) | BitsPerPixel(wic, frame.Get());
        if (score > best) {
            best = score;
            out = frame;
        }
    }
    return hr;
}

HRESULT DecodeImage(const wchar_t* path, Image& out) {
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) {
        hr = wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = GetFrame(wic.Get(), decoder.Get(), frame);
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width == 0 || height == 0) return WINCODEC_ERR_BADIMAGE;

    ComPtr<IWICBitmapSource> source = frame;
    if (width > kMaxBitmapSize || height > kMaxBitmapSize) {
        // Scaling at the decoder lets codecs like JPEG decode at reduced size directly.
        const double scale = double(kMaxBitmapSize) / std::max(width, height);
        width = std::max(1u, UINT(width * scale));
        height = std::max(1u, UINT(height * scale));
        ComPtr<IWICBitmapScaler> scaler;
        hr = wic->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return hr;
        source = scaler;
    }

    // EXIF orientation -> WIC transform (WIC flips first, then rotates clockwise).
    static constexpr WICBitmapTransformOptions kTransforms[] = {
        WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
        WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
        WICBitmapTransformOptions(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate90,
        WICBitmapTransformOptions(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate270,
    };
    const int orientation = ReadOrientation(frame.Get());
    if (orientation != 1) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = wic->CreateBitmapFlipRotator(&rotator);
        if (SUCCEEDED(hr)) hr = rotator->Initialize(source.Get(), kTransforms[orientation]);
        if (FAILED(hr)) return hr;
        if (orientation >= 5) std::swap(width, height);
        source = rotator;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) return hr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = LONG(width);
    bmi.bmiHeader.biHeight = -LONG(height);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return E_OUTOFMEMORY;

    const UINT stride = width * 4;
    hr = converter->CopyPixels(nullptr, stride, stride * height, static_cast<BYTE*>(bits));
    if (FAILED(hr)) {
        DeleteObject(bitmap);
        return hr;
    }

    // Composite transparent pixels over the background once, so painting is a plain blit.
    const UINT bg = kBackground & 0xFF;  // gray: all channels equal
    BYTE* p = static_cast<BYTE*>(bits);
    for (const BYTE* end = p + size_t(stride) * height; p != end; p += 4) {
        const UINT inverse = 255u - p[3];
        if (inverse == 0) continue;
        const BYTE add = BYTE((bg * inverse + 127) / 255);
        p[0] = BYTE(p[0] + add);
        p[1] = BYTE(p[1] + add);
        p[2] = BYTE(p[2] + add);
    }

    out.bitmap = bitmap;
    out.width = width;
    out.height = height;
    return S_OK;
}

struct Viewer {
    HWND hwnd = nullptr;
    std::wstring path;
    std::wstring name;

    Image image;
    HDC imageDC = nullptr;  // memory DC holding image.bitmap
    HBRUSH background = nullptr;

    float imageW = 0, imageH = 0;  // upright size in pixels
    float clientW = 0, clientH = 0;
    float zoom = 1, offsetX = 0, offsetY = 0;  // offset = image top-left in client pixels
    bool fit = true;
    bool dragging = false;
    POINT dragFrom{};
    long shownPercent = -1;
} g;

void Render(HDC dc) {
    // Only stretch the part of the image that is on screen.
    const float z = g.zoom;
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
    const RECT client{0, 0, LONG(g.clientW), LONG(g.clientH)};
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
    const long percent = std::lround(g.zoom * 100);
    if (percent == g.shownPercent) return;
    g.shownPercent = percent;
    SetWindowTextW(g.hwnd, (g.name + L"  ·  " + std::to_wstring(percent) + L"%").c_str());
}

void ZoomAt(float zoom, float x, float y) {
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

// Sizes the window to the image (capped to the work area) and centers it on the active monitor.
void PlaceWindow() {
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const float workW = float(work.right - work.left), workH = float(work.bottom - work.top);
    const float scale = std::min({1.0f, workW * 0.85f / g.imageW, workH * 0.85f / g.imageH});

    const UINT dpi = GetDpiForWindow(g.hwnd);
    RECT rc{0, 0, std::max(LONG(std::lround(g.imageW * scale)), LONG(MulDiv(480, dpi, 96))),
            std::max(LONG(std::lround(g.imageH * scale)), LONG(MulDiv(320, dpi, 96)))};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(g.hwnd, nullptr, work.left + (int(workW) - w) / 2, work.top + (int(workH) - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            Render(BeginPaint(hwnd, &ps));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            OnSize(float(LOWORD(lp)), float(HIWORD(lp)));
            return 0;
        case WM_MOUSEWHEEL: {  // also delivered for touchpad pinch
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
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
                g.offsetX += float(pt.x - g.dragFrom.x);
                g.offsetY += float(pt.y - g.dragFrom.y);
                g.dragFrom = pt;
                ClampOffset();
                InvalidateRect(hwnd, nullptr, FALSE);
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
                   float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)));
            return 0;
        }
        case WM_KEYDOWN: {
            const float cx = g.clientW / 2, cy = g.clientH / 2;
            switch (wp) {
                case VK_OEM_PLUS: case VK_ADD: ZoomAt(g.zoom * kZoomStep, cx, cy); break;
                case VK_OEM_MINUS: case VK_SUBTRACT: ZoomAt(g.zoom / kZoomStep, cx, cy); break;
                case '0': case VK_NUMPAD0: ZoomAt(FitZoom(), cx, cy); break;
                case '1': case VK_NUMPAD1: ZoomAt(1.0f, cx, cy); break;
                case VK_ESCAPE: DestroyWindow(hwnd); break;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
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

HWND CreateMainWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g.background = CreateSolidBrush(kBackground);
    wc.lpszClassName = kAppId;
    if (!RegisterClassExW(&wc)) return nullptr;

    HWND hwnd = CreateWindowExW(0, kAppId, kAppName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, nullptr);
    if (hwnd) {
        // Dark title bar that blends into the canvas (caption color is Windows 11 only).
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF caption = kBackground;
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    }
    return hwnd;
}

std::wstring Unquote(const wchar_t* s) {
    std::wstring arg(s ? s : L"");
    const auto first = arg.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    arg = arg.substr(first, arg.find_last_not_of(L" \t") - first + 1);
    if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
    return arg;
}

bool PickFile(std::wstring& path) {
    wchar_t buffer[32768] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.lpstrFilter = kFileFilter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = ARRAYSIZE(buffer);
    ofn.lpstrTitle = kAppName;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    path = buffer;
    return true;
}

LSTATUS SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           DWORD((value.size() + 1) * sizeof(wchar_t)));
}

// Registers per user as a candidate image viewer. Windows only lets the user pick
// the default, so this then opens the Default apps page for this app.
int Register() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (len == 0 || len == ARRAYSIZE(exe)) return 1;

    const std::wstring progKey = std::wstring(L"Software\\Classes\\") + kProgId;
    const std::wstring capsKey = std::wstring(L"Software\\") + kAppId + L"\\Capabilities";
    LSTATUS status = SetString(progKey, nullptr, L"Image");
    status |= SetString(progKey + L"\\DefaultIcon", nullptr, L"\"" + std::wstring(exe) + L"\",0");
    status |= SetString(progKey + L"\\shell\\open\\command", nullptr, L"\"" + std::wstring(exe) + L"\" \"%1\"");
    status |= SetString(capsKey, L"ApplicationName", kAppName);
    status |= SetString(capsKey, L"ApplicationDescription", L"A fast, minimal image viewer.");
    status |= SetString(L"Software\\RegisteredApplications", kAppId, capsKey);
    for (const wchar_t* ext : kExtensions) {
        status |= SetString(capsKey + L"\\FileAssociations", ext, kProgId);
        status |= RegSetKeyValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(),
                                  kProgId, REG_NONE, nullptr, 0);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (status != ERROR_SUCCESS) return 1;

    ShellExecuteW(nullptr, L"open", (std::wstring(L"ms-settings:defaultapps?registeredAppUser=") + kAppId).c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

int Unregister() {
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProgId).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\") + kAppId).c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppId);
    for (const wchar_t* ext : kExtensions) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids").c_str(),
                           kProgId);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return 0;
}

void ShowError(HRESULT hr) {
    std::wstring message = L"Can't open \"" + g.name + L"\".";
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND) {
        message += L"\n\nThis format isn't supported. Codecs for HEIC/AVIF are available from the Microsoft Store.";
    } else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        message += L"\n\nThe file doesn't exist.";
    }
    MessageBoxW(nullptr, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int showCmd) {
    g.path = Unquote(cmdLine);
    if (g.path == L"--register") return Register();
    if (g.path == L"--unregister") return Unregister();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    if (g.path.empty() && !PickFile(g.path)) return 0;
    g.name = g.path.substr(g.path.find_last_of(L"\\/") + 1);

    // Decode on a worker thread while the window is set up.
    HRESULT decodeHr = E_FAIL;
    std::thread decoder([&] {
        decodeHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(decodeHr)) {
            decodeHr = DecodeImage(g.path.c_str(), g.image);
            CoUninitialize();
        }
    });

    g.hwnd = CreateMainWindow(instance);
    decoder.join();
    if (FAILED(decodeHr)) {
        ShowError(decodeHr);
        return 1;
    }
    if (!g.hwnd) return 1;

    g.imageDC = CreateCompatibleDC(nullptr);
    if (!g.imageDC || !SelectObject(g.imageDC, g.image.bitmap)) return 1;
    g.imageW = float(g.image.width);
    g.imageH = float(g.image.height);

    PlaceWindow();
    ShowWindow(g.hwnd, showCmd);
    UpdateWindow(g.hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
