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
#include <memory>
#include <new>
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
constexpr COLORREF kBackground = 0x000000;  // black
constexpr COLORREF kChrome = 0x202020;      // title bar and menu bar, Windows dark gray
constexpr COLORREF kChromeHover = 0x2D2D2D;
constexpr COLORREF kChromePressed = 0x383838;
constexpr COLORREF kChromeText = 0xE6E6E6;
constexpr float kMaxZoom = 64.0f;
constexpr float kZoomStep = 1.25f;

enum Command : UINT { kCmdNew = 1, kCmdOpen, kCmdSaveAs };

struct Image {
    HBITMAP bitmap = nullptr;  // 24bpp or palettized top-down DIB section, composited over kBackground
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

// Creates a top-down DIB section; colors is the color table for bpp <= 8.
HBITMAP CreateDib(UINT width, UINT height, WORD bpp, const UINT32* colors, UINT count, BYTE*& bits) {
    struct {
        BITMAPINFOHEADER header;
        UINT32 colors[256];  // RGBQUAD layout
    } bmi{};
    bmi.header.biSize = sizeof(bmi.header);
    bmi.header.biWidth = LONG(width);
    bmi.header.biHeight = -LONG(height);  // top-down
    bmi.header.biPlanes = 1;
    bmi.header.biBitCount = bpp;
    bmi.header.biCompression = BI_RGB;
    bmi.header.biClrUsed = count;
    std::copy_n(colors, count, bmi.colors);
    void* p = nullptr;
    HBITMAP bitmap =
        CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &p, nullptr, 0);
    bits = static_cast<BYTE*>(p);
    return bitmap;
}

// Maps up to 256 distinct 0x00RRGGBB colors to palette indices.
struct Palette {
    static constexpr UINT32 kUsed = 0x1000000;
    UINT32 colors[256];
    UINT count = 0;
    UINT32 keys[512] = {};  // color | kUsed, 0 = empty slot
    BYTE indices[512];

    // Returns the color's index, adding it if new, or -1 once the palette is full.
    int Find(UINT32 color) {
        UINT i = (color * 0x9E3779B1u) >> 23;
        for (; keys[i] != 0; i = (i + 1) & 511) {
            if (keys[i] == (color | kUsed)) return indices[i];
        }
        if (count == 256) return -1;
        keys[i] = color | kUsed;
        indices[i] = BYTE(count);
        colors[count] = color;
        return int(count++);
    }
};

// Where an image comes from: a file, or an in-memory copy of pasted data.
struct Source {
    std::wstring path;
    ComPtr<IStream> stream;
};

// Opens the upright image: the chosen frame, scaled to fit maxSize (0 = no limit), EXIF orientation applied.
HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize, ComPtr<IWICBitmapSource>& source,
                   UINT& width, UINT& height) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr;
    if (src.stream) {
        hr = src.stream->Seek({}, STREAM_SEEK_SET, nullptr);
        if (SUCCEEDED(hr)) {
            hr = wic->CreateDecoderFromStream(src.stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        }
    } else {
        hr = wic->CreateDecoderFromFilename(src.path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = GetFrame(wic, decoder.Get(), frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width == 0 || height == 0) return WINCODEC_ERR_BADIMAGE;

    source = frame;
    if (maxSize && (width > maxSize || height > maxSize)) {
        // Scaling at the decoder lets codecs like JPEG decode at reduced size directly.
        const double scale = double(maxSize) / std::max(width, height);
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
        // The rotator reads its source pixel by pixel, so buffer it to keep strip reads below linear.
        ComPtr<IWICBitmap> buffered;
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = wic->CreateBitmapFromSource(source.Get(), WICBitmapCacheOnLoad, &buffered);
        if (SUCCEEDED(hr)) hr = wic->CreateBitmapFlipRotator(&rotator);
        if (SUCCEEDED(hr)) hr = rotator->Initialize(buffered.Get(), kTransforms[orientation]);
        if (FAILED(hr)) return hr;
        if (orientation >= 5) std::swap(width, height);
        source = rotator;
    }
    return S_OK;
}

HRESULT DecodeImage(const Source& src, Image& out) {
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic));
    ComPtr<IWICBitmapSource> source;
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = OpenSource(wic.Get(), src, kMaxBitmapSize, source, width, height);
    if (FAILED(hr)) return hr;

    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) return hr;

    // Decode in strips through a small 32bpp buffer and store the exact pixels in the smallest form:
    // palette indices while the image has <= 256 colors, otherwise 24bpp (alpha is composited away).
    const UINT stripStride = width * 4;
    const UINT rows = std::clamp((1u << 20) / stripStride, 1u, height);
    const std::unique_ptr<BYTE[]> strip(new (std::nothrow) BYTE[size_t(stripStride) * rows]);
    std::unique_ptr<BYTE[]> indices(new (std::nothrow) BYTE[size_t(width) * height]);
    if (!strip) return E_OUTOFMEMORY;

    const UINT stride24 = (width * 3 + 3) & ~3u;
    Palette palette;
    UINT32 lastColor = ~0u;
    BYTE lastIndex = 0;
    BYTE* bits = nullptr;
    HBITMAP bitmap = indices ? nullptr : CreateDib(width, height, 24, nullptr, 0, bits);
    if (!indices && !bitmap) return E_OUTOFMEMORY;

    const UINT bg = kBackground & 0xFF;  // gray: all channels equal
    for (UINT y0 = 0; y0 < height; y0 += rows) {
        const UINT n = std::min(rows, height - y0);
        const WICRect rect{0, INT(y0), INT(width), INT(n)};
        hr = converter->CopyPixels(&rect, stripStride, stripStride * n, strip.get());
        if (FAILED(hr)) break;
        BYTE* const begin = strip.get();
        const BYTE* const end = begin + size_t(stripStride) * n;

        // Composite transparent pixels over the background once, so painting is a plain blit.
        for (BYTE* p = begin; p != end; p += 4) {
            const UINT inverse = 255u - p[3];
            if (inverse == 0) continue;
            const BYTE add = BYTE((bg * inverse + 127) / 255);
            p[0] = BYTE(p[0] + add);
            p[1] = BYTE(p[1] + add);
            p[2] = BYTE(p[2] + add);
        }

        if (indices) {
            BYTE* dst = indices.get() + size_t(y0) * width;
            const BYTE* p = begin;
            for (; p != end; p += 4) {
                const UINT32 color = p[0] | UINT32(p[1]) << 8 | UINT32(p[2]) << 16;
                if (color != lastColor) {
                    const int index = palette.Find(color);
                    if (index < 0) break;
                    lastColor = color;
                    lastIndex = BYTE(index);
                }
                *dst++ = lastIndex;
            }
            if (p == end) continue;

            // More than 256 colors: switch to 24bpp, expanding the rows stored so far.
            bitmap = CreateDib(width, height, 24, nullptr, 0, bits);
            if (!bitmap) {
                hr = E_OUTOFMEMORY;
                break;
            }
            for (UINT y = 0; y < y0; ++y) {
                const BYTE* in = indices.get() + size_t(y) * width;
                BYTE* d = bits + size_t(y) * stride24;
                for (UINT x = 0; x < width; ++x, d += 3) {
                    const UINT32 color = palette.colors[in[x]];
                    d[0] = BYTE(color);
                    d[1] = BYTE(color >> 8);
                    d[2] = BYTE(color >> 16);
                }
            }
            indices.reset();
        }

        for (UINT y = 0; y < n; ++y) {
            const BYTE* p = begin + size_t(y) * stripStride;
            BYTE* d = bits + size_t(y0 + y) * stride24;
            for (UINT x = 0; x < width; ++x, p += 4, d += 3) {
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
            }
        }
    }
    if (FAILED(hr)) {
        if (bitmap) DeleteObject(bitmap);
        return hr;
    }

    if (indices) {
        const WORD bpp = palette.count <= 2 ? 1 : palette.count <= 16 ? 4 : 8;
        bitmap = CreateDib(width, height, bpp, palette.colors, palette.count, bits);
        if (!bitmap) return E_OUTOFMEMORY;
        const UINT stride = (width * bpp + 31) / 32 * 4;
        const UINT perByte = 8u / bpp;
        for (UINT y = 0; y < height; ++y) {
            const BYTE* in = indices.get() + size_t(y) * width;
            BYTE* d = bits + size_t(y) * stride;
            if (bpp == 8) {
                std::copy_n(in, width, d);
                continue;
            }
            BYTE packed = 0;  // leftmost pixel in the high bits
            UINT k = 0;
            for (UINT x = 0; x < width; ++x) {
                packed = BYTE(packed << bpp | in[x]);
                if (++k == perByte) {
                    *d++ = packed;
                    packed = 0;
                    k = 0;
                }
            }
            if (k) *d = BYTE(packed << (bpp * (perByte - k)));
        }
    }

    out.bitmap = bitmap;
    out.width = width;
    out.height = height;
    return S_OK;
}

struct Viewer {
    HWND hwnd = nullptr;
    HWND bar = nullptr;  // menu bar child window across the top of the client area
    HMENU fileMenu = nullptr;
    HFONT barFont = nullptr;
    int barH = 0;
    RECT fileButton{};
    bool fileHot = false, menuOpen = false;
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

bool PickFile(std::wstring& path) {
    wchar_t buffer[32768] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = kFileFilter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = ARRAYSIZE(buffer);
    ofn.lpstrTitle = kAppName;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    path = buffer;
    return true;
}

void ShowError(HRESULT hr, const std::wstring& name) {
    std::wstring message = L"Can't open \"" + name + L"\".";
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND) {
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
    EnableMenuItem(g.fileMenu, kCmdNew, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(g.fileMenu, kCmdSaveAs, MF_BYCOMMAND | MF_ENABLED);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Closes the current image, leaving the empty window.
void CloseImage() {
    if (!g.image.bitmap) return;
    SelectObject(g.imageDC, g.emptyBitmap);
    DeleteObject(g.image.bitmap);
    g.image = {};
    g.source = {};
    g.name.clear();
    g.imageW = g.imageH = 0;
    g.zoom = 1;
    g.fit = true;
    g.shownPercent = -1;
    ClampOffset();
    SetWindowTextW(g.hwnd, kAppName);
    EnableMenuItem(g.fileMenu, kCmdNew, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(g.fileMenu, kCmdSaveAs, MF_BYCOMMAND | MF_GRAYED);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Decodes and shows an image, keeping the current one if that fails.
void Load(Source src, std::wstring name) {
    Image image;
    const HRESULT hr = DecodeImage(src, image);
    if (FAILED(hr)) {
        ShowError(hr, name);
        return;
    }
    g.source = std::move(src);
    SetImage(image, std::move(name));
}

void Open() {
    std::wstring path;
    if (!PickFile(path)) return;
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    Load({std::move(path)}, std::move(name));
}

// Copies the clipboard image into a stream: PNG if offered (lossless, keeps alpha), else the DIB
// as a BMP file. Returns S_FALSE if the clipboard holds no image.
HRESULT CopyClipboardImage(ComPtr<IStream>& out) {
    static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (!OpenClipboard(g.hwnd)) return HRESULT_FROM_WIN32(GetLastError());
    const bool png = IsClipboardFormatAvailable(pngFormat);
    const HGLOBAL data = GetClipboardData(png ? pngFormat : CF_DIB);
    const BYTE* p = data ? static_cast<const BYTE*>(GlobalLock(data)) : nullptr;
    if (!p) {
        CloseClipboard();
        return S_FALSE;
    }

    const SIZE_T size = GlobalSize(data);
    HRESULT hr = S_OK;
    BITMAPFILEHEADER file{};
    if (!png) {
        // The clipboard is shared with other programs, so check the header fits before trusting it.
        const auto* info = reinterpret_cast<const BITMAPINFOHEADER*>(p);
        if (size < sizeof(BITMAPINFOHEADER) || info->biSize < sizeof(BITMAPINFOHEADER) || info->biSize > size) {
            hr = WINCODEC_ERR_BADIMAGE;
        } else {
            const UINT64 masks =
                info->biSize == sizeof(BITMAPINFOHEADER) && info->biCompression == BI_BITFIELDS ? 12 : 0;
            const UINT64 colors =
                info->biClrUsed ? info->biClrUsed : info->biBitCount <= 8 ? 1u << info->biBitCount : 0;
            const UINT64 offset = info->biSize + masks + colors * 4;
            if (offset > size || size > MAXDWORD - sizeof(file)) {
                hr = WINCODEC_ERR_BADIMAGE;
            } else {
                file.bfType = 0x4D42;  // "BM"
                file.bfSize = DWORD(sizeof(file) + size);
                file.bfOffBits = DWORD(sizeof(file) + offset);
            }
        }
    }

    const SIZE_T header = png ? 0 : sizeof(file);
    HGLOBAL copy = SUCCEEDED(hr) ? GlobalAlloc(GMEM_MOVEABLE, header + size) : nullptr;
    if (SUCCEEDED(hr) && !copy) hr = E_OUTOFMEMORY;
    if (copy) {
        if (BYTE* d = static_cast<BYTE*>(GlobalLock(copy))) {
            std::copy_n(reinterpret_cast<const BYTE*>(&file), header, d);
            std::copy_n(p, size, d + header);
            GlobalUnlock(copy);
            hr = CreateStreamOnHGlobal(copy, TRUE, &out);
        } else {
            hr = E_OUTOFMEMORY;
        }
        if (FAILED(hr)) GlobalFree(copy);
    }
    GlobalUnlock(data);
    CloseClipboard();
    return hr;
}

// Re-reads the current image's source at full size and quality and encodes it as PNG, or JPEG
// for a .jpg/.jpeg path. Writes a temporary file first so a failure never leaves a partial file
// and the source itself can be overwritten.
HRESULT SaveImage(const std::wstring& path) {
    const size_t dot = path.find_last_of(L".\\/");
    const wchar_t* ext = dot != std::wstring::npos && path[dot] == L'.' ? path.c_str() + dot : L"";
    const bool jpeg = CompareStringOrdinal(ext, -1, L".jpg", -1, TRUE) == CSTR_EQUAL ||
                      CompareStringOrdinal(ext, -1, L".jpeg", -1, TRUE) == CSTR_EQUAL;
    const std::wstring temp = path + L".tmp";

    HRESULT hr;
    {
        ComPtr<IWICImagingFactory> wic;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        ComPtr<IWICBitmapSource> source;
        UINT width = 0, height = 0;
        if (SUCCEEDED(hr)) hr = OpenSource(wic.Get(), g.source, 0, source, width, height);
        ComPtr<IWICStream> stream;
        if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) return hr;

        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        hr = wic->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
        if (SUCCEEDED(hr) && jpeg) {  // best quality, no chroma subsampling
            PROPBAG2 names[2] = {};
            names[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            names[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
            VARIANT values[2] = {};
            values[0].vt = VT_R4;
            values[0].fltVal = 1.0f;
            values[1].vt = VT_UI1;
            values[1].bVal = WICJpegYCrCbSubsampling444;
            hr = options->Write(2, names, values);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);

        // Keep the source's pixel format (bit depth, alpha, palette) where the encoder supports it.
        WICPixelFormatGUID sourceFormat{}, format{};
        if (SUCCEEDED(hr)) hr = source->GetPixelFormat(&sourceFormat);
        format = sourceFormat;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != sourceFormat) {
            ComPtr<IWICBitmapSource> converted;
            hr = WICConvertBitmapSource(format, source.Get(), &converted);
            source = converted;
        } else if (SUCCEEDED(hr)) {
            ComPtr<IWICPalette> palette;
            if (SUCCEEDED(wic->CreatePalette(&palette)) && SUCCEEDED(source->CopyPalette(palette.Get()))) {
                hr = frame->SetPalette(palette.Get());
            }
        }
        if (SUCCEEDED(hr)) hr = frame->WriteSource(source.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
    }  // closes the temporary file

    if (SUCCEEDED(hr) && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(hr)) DeleteFileW(temp.c_str());
    return hr;
}

void SaveAs() {
    if (!g.image.bitmap) return;
    wchar_t buffer[32768] = L"";
    const std::wstring stem = g.name.substr(0, g.name.find_last_of(L'.'));
    stem.copy(buffer, std::min(stem.size(), ARRAYSIZE(buffer) - 1));
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"PNG\0*.png\0JPEG\0*.jpg;*.jpeg\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = ARRAYSIZE(buffer);
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    if (FAILED(SaveImage(buffer))) {
        const std::wstring message = L"Can't save \"" + std::wstring(buffer) + L"\".";
        MessageBoxW(g.hwnd, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
    }
}

void Paste() {
    Source src;
    const HRESULT hr = CopyClipboardImage(src.stream);
    if (hr == S_OK) {
        Load(std::move(src), L"Pasted image");
    } else if (FAILED(hr)) {
        ShowError(hr, L"Pasted image");
    }
}

// Sizes the menu bar and its font for a DPI.
void LayoutBar(UINT dpi) {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    if (g.barFont) DeleteObject(g.barFont);
    g.barFont = CreateFontIndirectW(&ncm.lfMenuFont);

    SIZE text{};
    HDC dc = GetDC(nullptr);
    const HGDIOBJ old = SelectObject(dc, g.barFont);
    GetTextExtentPoint32W(dc, L"File", 4, &text);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);

    const int pad = MulDiv(4, dpi, 96);
    g.barH = MulDiv(32, dpi, 96);
    g.fileButton = {pad, pad, pad + text.cx + MulDiv(20, dpi, 96), g.barH - pad};
}

void PaintBar(HDC dc) {
    RECT client;
    GetClientRect(g.bar, &client);
    SetDCBrushColor(dc, kChrome);
    FillRect(dc, &client, HBRUSH(GetStockObject(DC_BRUSH)));
    const RECT& r = g.fileButton;
    if (g.menuOpen || g.fileHot) {
        const COLORREF color = g.menuOpen ? kChromePressed : kChromeHover;
        SetDCBrushColor(dc, color);
        SetDCPenColor(dc, color);
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        const int radius = g.barH / 4;
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    }
    SelectObject(dc, g.barFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kChromeText);
    RECT text = r;
    DrawTextW(dc, L"File", 4, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void ShowFileMenu(bool keyboard) {
    g.menuOpen = true;
    RedrawWindow(g.bar, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    RECT r = g.fileButton;
    MapWindowPoints(g.bar, nullptr, reinterpret_cast<POINT*>(&r), 2);
    TPMPARAMS params{sizeof(params), r};
    if (keyboard) PostMessageW(g.hwnd, WM_KEYDOWN, VK_DOWN, 0);  // select the first item
    TrackPopupMenuEx(g.fileMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, r.left, r.bottom, g.hwnd, &params);
    // The click that closed the menu must not reopen it.
    MSG msg;
    PeekMessageW(&msg, g.bar, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE);
    g.menuOpen = false;
    g.fileHot = false;
    InvalidateRect(g.bar, nullptr, FALSE);
}

LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            PaintBar(BeginPaint(hwnd, &ps));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            const bool hot = PtInRect(&g.fileButton, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot != g.fileHot) {
                g.fileHot = hot;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd};
                if (hot) TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g.fileHot = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            if (PtInRect(&g.fileButton, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) ShowFileMenu(false);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            LayoutBar(GetDpiForWindow(hwnd));
            g.bar = CreateWindowExW(0, L"ImageViewerBar", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                    nullptr, reinterpret_cast<const CREATESTRUCTW*>(lp)->hInstance, nullptr);
            return g.bar ? 0 : -1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(hwnd, &ps);
            SetViewportOrgEx(dc, 0, g.barH, nullptr);  // the image area starts below the bar
            Render(dc);
            EndPaint(hwnd, &ps);
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
                case VK_ESCAPE: DestroyWindow(hwnd); break;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            LayoutBar(HIWORD(wp));
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

    WNDCLASSEXW bar{sizeof(bar)};
    bar.lpfnWndProc = BarProc;
    bar.hInstance = instance;
    bar.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    bar.lpszClassName = L"ImageViewerBar";
    if (!RegisterClassExW(&bar)) return nullptr;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g.background = CreateSolidBrush(kBackground);
    wc.lpszClassName = kAppId;
    if (!RegisterClassExW(&wc)) return nullptr;

    g.fileMenu = CreatePopupMenu();
    AppendMenuW(g.fileMenu, MF_STRING | MF_GRAYED, kCmdNew, L"&New\tCtrl+N");
    AppendMenuW(g.fileMenu, MF_STRING, kCmdOpen, L"&Open…\tCtrl+O");
    AppendMenuW(g.fileMenu, MF_STRING | MF_GRAYED, kCmdSaveAs, L"Save &As…\tCtrl+S");

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

std::wstring Unquote(const wchar_t* s) {
    std::wstring arg(s ? s : L"");
    const auto first = arg.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    arg = arg.substr(first, arg.find_last_not_of(L" \t") - first + 1);
    if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
    return arg;
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

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int showCmd) {
    g.source.path = Unquote(cmdLine);
    if (g.source.path == L"--register") return Register();
    if (g.source.path == L"--unregister") return Unregister();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    g.name = g.source.path.substr(g.source.path.find_last_of(L"\\/") + 1);

    // Decode on a worker thread while the window is set up.
    Image image;
    HRESULT decodeHr = S_OK;
    std::thread decoder;
    if (!g.source.path.empty()) {
        decoder = std::thread([&] {
            decodeHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(decodeHr)) {
                decodeHr = DecodeImage(g.source, image);
                CoUninitialize();
            }
        });
    }

    g.hwnd = CreateMainWindow(instance);
    if (decoder.joinable()) decoder.join();
    if (FAILED(decodeHr)) {
        ShowError(decodeHr, g.name);
        return 1;
    }
    if (!g.hwnd) return 1;

    g.imageDC = CreateCompatibleDC(nullptr);
    if (!g.imageDC) return 1;
    g.emptyBitmap = GetCurrentObject(g.imageDC, OBJ_BITMAP);
    if (image.bitmap) SetImage(image, g.name);

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
