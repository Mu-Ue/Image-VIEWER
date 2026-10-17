// Checks that what the viewer shows and saves is exact. Build with "cmake --build build --target tests" and
// run build\tests.exe; it prints each failure and exits with 1 if there was any.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "imaging/animation.h"
#include "imaging/decoder.h"
#include "imaging/encoder.h"
#include "ui/scale.h"

// Included for its parsers of clipboard data, which other programs control.
#include "platform/clipboard.cpp"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void Fail(const char* what, int value) {
    printf("FAIL %s (%d)\n", what, value);
    ++failures;
}

// A top-down DIB section of random pixels: 24bpp, or palette indices with a random color table.
HBITMAP RandomDib(std::mt19937& rng, int w, int h, int bpp, RGBQUAD (&colors)[256]) {
    struct {
        BITMAPINFOHEADER header;
        RGBQUAD colors[256];
    } bmi{{sizeof(BITMAPINFOHEADER), w, -h, 1, WORD(bpp), BI_RGB}};
    const int count = bpp <= 8 ? 1 << bpp : 0;
    for (int i = 0; i < count; ++i) colors[i] = bmi.colors[i] = {BYTE(rng()), BYTE(rng()), BYTE(rng()), 0};
    bmi.header.biClrUsed = count;
    void* bits = nullptr;
    const HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &bits, nullptr, 0);
    const size_t stride = (size_t(w) * bpp + 31) / 32 * 4;
    for (size_t i = 0; i < stride * h; ++i) static_cast<BYTE*>(bits)[i] = BYTE(rng());
    return dib;
}

// Source pixel (x, y) of a DIB as 0x00RRGGBB.
UINT32 Pixel(const DIBSECTION& ds, const RGBQUAD* colors, int x, int y) {
    const BYTE* row = static_cast<const BYTE*>(ds.dsBm.bmBits) + size_t(y) * ds.dsBm.bmWidthBytes;
    const int bpp = ds.dsBm.bmBitsPixel;
    if (bpp == 24) return row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16;
    const int bit = x * bpp;
    const RGBQUAD& q = colors[(row[bit / 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1)];
    return q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16;
}

// Output pixel (i, j) of a w x h source shown as ow x oh: the exact average of the area it covers, rounded
// half up, computed the slow way.
UINT32 Reference(const DIBSECTION& ds, const RGBQUAD* colors, int ow, int oh, int i, int j) {
    const UINT64 w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    UINT64 sum[3] = {};
    // In units of 1/ow (1/oh) source pixel, output pixel i spans [i * w, (i + 1) * w).
    for (UINT64 y = j * h / oh; y < h && y * oh < (j + 1) * h; ++y) {
        const UINT64 wy = std::min((y + 1) * oh, (j + 1) * h) - std::max(y * oh, j * h);
        for (UINT64 x = i * w / ow; x < w && x * ow < (i + 1) * w; ++x) {
            const UINT64 wx = std::min((x + 1) * ow, (i + 1) * w) - std::max(x * ow, i * w);
            const UINT32 c = Pixel(ds, colors, int(x), int(y));
            for (int k = 0; k < 3; ++k) sum[k] += wx * wy * (c >> (8 * k) & 0xFF);
        }
    }
    const UINT64 area = w * h;
    return UINT32((sum[0] + area / 2) / area | (sum[1] + area / 2) / area << 8 | (sum[2] + area / 2) / area << 16);
}

// DrawScaled against the reference for random sizes, zooms, positions, clips and bit depths.
void TestScaling() {
    std::mt19937 rng(7);
    const int outW = 640, outH = 480;
    BITMAPINFO target{{sizeof(BITMAPINFOHEADER), outW, -outH, 1, 32, BI_RGB}};
    void* out = nullptr;
    const HBITMAP screen = CreateDIBSection(nullptr, &target, DIB_RGB_COLORS, &out, nullptr, 0);
    const HDC dc = CreateCompatibleDC(nullptr), imageDC = CreateCompatibleDC(nullptr);
    SelectObject(dc, screen);
    constexpr int kDepths[] = {24, 24, 8, 4, 1};
    for (int t = 0; t < 400; ++t) {
        const int bpp = kDepths[rng() % 5];
        const int w = 1 + int(rng() % 300), h = 1 + int(rng() % 300);
        RGBQUAD colors[256];
        const HBITMAP src = RandomDib(rng, w, h, bpp, colors);
        SelectObject(imageDC, src);
        DIBSECTION ds;
        GetObjectW(src, sizeof(ds), &ds);
        // Zooms from 1/50 to 8x, sometimes exactly 100%.
        const double zoom = t % 10 == 0 ? 1.0 : std::exp(std::uniform_real_distribution<double>(-3.9, 2.1)(rng));
        const int ow = std::max(1, int(w * zoom + 0.5)), oh = std::max(1, int(h * zoom + 0.5));
        const int x = int(rng() % 200) - 100, y = int(rng() % 200) - 100;
        const int cl = int(rng() % outW), ct = int(rng() % outH);
        const RECT clip{cl, ct, cl + 1 + int(rng() % (outW - cl)), ct + 1 + int(rng() % (outH - ct))};
        memset(out, 0xAB, size_t(outW) * outH * 4);
        if (!DrawScaled(dc, imageDC, x, y, ow, oh, clip)) Fail("DrawScaled refused a DIB", bpp);
        GdiFlush();
        int wrong = 0;
        for (int j = std::max(int(clip.top), y); j < std::min(int(clip.bottom), y + oh); ++j) {
            for (int i = std::max(int(clip.left), x); i < std::min(int(clip.right), x + ow); ++i) {
                const UINT32 got = static_cast<const UINT32*>(out)[size_t(j) * outW + i] & 0xFFFFFF;
                wrong += got != Reference(ds, colors, ow, oh, i - x, j - y);
            }
        }
        if (wrong) Fail("scaled pixels differ from the exact average", wrong);
        DeleteObject(src);
    }
    DeleteDC(imageDC);
    DeleteDC(dc);
    DeleteObject(screen);
}

std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"ImageVIEWER-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + name;
}

// Writes an image of random pixels in the given container and pixel format, with an EXIF orientation. For 24bpp,
// the first fewColorRows rows use only fewColors colors. profile, if given, is embedded.
HRESULT WriteImage(IWICImagingFactory* wic, const std::wstring& path, const GUID& container, WICPixelFormatGUID format,
                   UINT w, UINT h, int orientation, std::mt19937& rng, UINT fewColorRows = 0, UINT fewColors = 200,
                   IWICColorContext* profile = nullptr) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICMetadataQueryWriter> meta;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && profile) hr = frame->SetColorContexts(1, &profile);
    const bool indexed = format == GUID_WICPixelFormat8bppIndexed;
    if (SUCCEEDED(hr) && indexed) {
        ComPtr<IWICPalette> palette;
        WICColor colors[200];
        for (WICColor& c : colors) c = 0xFF000000 | (rng() & 0xFFFFFF);
        hr = wic->CreatePalette(&palette);
        if (SUCCEEDED(hr)) hr = palette->InitializeCustom(colors, 200);
        if (SUCCEEDED(hr)) hr = frame->SetPalette(palette.Get());
    }
    if (SUCCEEDED(hr) && orientation) {
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_UI2;
        value.uiVal = USHORT(orientation);
        hr = frame->GetMetadataQueryWriter(&meta);
        const wchar_t* query = container == GUID_ContainerFormatJpeg ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}";
        if (SUCCEEDED(hr)) hr = meta->SetMetadataByName(query, &value);
    }
    const UINT bpp = indexed ? 8 : format == GUID_WICPixelFormat32bppBGRA ? 32 : 24;
    const UINT stride = (w * bpp / 8 + 3) & ~3u;
    std::vector<BYTE> pixels(size_t(stride) * h);
    std::vector<UINT32> few(fewColorRows ? fewColors : 0);
    for (UINT32& c : few) c = rng() & 0xFFFFFF;
    for (UINT y = 0; y < h; ++y) {
        BYTE* const row = pixels.data() + size_t(y) * stride;
        if (y < fewColorRows) {
            for (UINT x = 0; x < w; ++x) memcpy(row + x * 3, &few[rng() % fewColors], 3);
        } else {
            for (UINT x = 0; x < w * bpp / 8; ++x) row[x] = BYTE(indexed ? rng() % 200 : rng());
        }
    }
    if (SUCCEEDED(hr)) hr = frame->WritePixels(h, stride, UINT(pixels.size()), pixels.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

// The upright 24bpp pixels WIC itself makes of a file, by its own flip/rotate.
std::vector<BYTE> WicUpright(IWICImagingFactory* wic, const std::wstring& path, int orientation, UINT& w, UINT& h) {
    static constexpr WICBitmapTransformOptions kTransforms[] = {
        WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
        WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
        WICBitmapTransformOptions(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate90,
        WICBitmapTransformOptions(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate270,
    };
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICBitmap> buffered;
    ComPtr<IWICBitmapFlipRotator> rotator;
    std::vector<BYTE> pixels;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &buffered)) ||
        FAILED(wic->CreateBitmapFlipRotator(&rotator)) ||
        FAILED(rotator->Initialize(buffered.Get(), kTransforms[orientation])) || FAILED(rotator->GetSize(&w, &h))) {
        return pixels;
    }
    const UINT stride = (w * 3 + 3) & ~3u;
    pixels.resize(size_t(stride) * h);
    rotator->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.data());
    return pixels;
}

// DecodeImage applies every EXIF orientation like WIC does, on its 24bpp and palette paths, and when it switches
// from one to the other partway through.
void TestOrientation(IWICImagingFactory* wic) {
    std::mt19937 rng(3);
    struct {
        const GUID& container;
        WICPixelFormatGUID format;
        const wchar_t* ext;
        UINT h, fewColorRows;
    } kinds[] = {{GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, L".jpg", 97, 0},
                 {GUID_ContainerFormatTiff, GUID_WICPixelFormat8bppIndexed, L".tif", 97, 0},
                 // Decoded in strips of 1747 rows, so the colors run out in the second one.
                 {GUID_ContainerFormatTiff, GUID_WICPixelFormat24bppBGR, L".tif", 4000, 2000}};
    for (int k = 0; k < int(std::size(kinds)); ++k) {
        const auto& kind = kinds[k];
        for (int orientation = 1; orientation <= 8; ++orientation) {
            const std::wstring path =
                TempPath((L"orientation" + std::to_wstring(k) + L"-" + std::to_wstring(orientation) + kind.ext).c_str());
            // Wider than the 64-pixel tiles sideways images are written in, and odd-sized.
            if (FAILED(WriteImage(wic, path, kind.container, kind.format, 150, kind.h, orientation, rng,
                                  kind.fewColorRows))) {
                Fail("can't write an orientation test image", orientation);
                continue;
            }
            Image image;
            UINT w = 0, h = 0;
            const std::vector<BYTE> want = WicUpright(wic, path, orientation, w, h);
            if (FAILED(DecodeImage(Source{path}, image)) || want.empty() || image.width != w || image.height != h) {
                Fail("decoded orientation has the wrong size", orientation);
            } else {
                const HDC dc = CreateCompatibleDC(nullptr);
                BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), LONG(w), -LONG(h), 1, 24, BI_RGB}};
                std::vector<BYTE> got(want.size());
                GetDIBits(dc, image.bitmap, 0, h, got.data(), &bi, DIB_RGB_COLORS);
                DeleteDC(dc);
                if (got != want) Fail("decoded orientation differs from WIC's", orientation);
            }
            if (image.bitmap) DeleteObject(image.bitmap);
            DeleteFileW(path.c_str());
        }
    }
}

std::vector<BYTE> ReadAll(const std::wstring& path) {
    std::vector<BYTE> data;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size;
    DWORD n = 0;
    if (GetFileSizeEx(file, &size)) {
        data.resize(size_t(size.QuadPart));
        ReadFile(file, data.data(), DWORD(data.size()), &n, nullptr);
    }
    CloseHandle(file);
    return data;
}

// Save As copies a file saved in its own format, and composites alpha over white for JPEG.
void TestSave(IWICImagingFactory* wic) {
    std::mt19937 rng(5);
    const std::wstring jpg = TempPath(L"save.jpg"), copy = TempPath(L"save copy.jpg");
    if (FAILED(WriteImage(wic, jpg, GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, 64, 48, 6, rng)) ||
        FAILED(SaveImage(Source{jpg}, copy)) || ReadAll(jpg) != ReadAll(copy)) {
        Fail("saving a JPEG as JPEG didn't copy it", 0);
    }

    // A PNG whose fully transparent pixels hide red.
    const std::wstring png = TempPath(L"alpha.png"), flat = TempPath(L"alpha.jpg");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    std::vector<UINT32> red(64 * 48, 0x00FF0000);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(png.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(64, 48);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = frame->WritePixels(48, 64 * 4, UINT(red.size() * 4), reinterpret_cast<BYTE*>(red.data()));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    frame.Reset();
    encoder.Reset();
    stream.Reset();  // closes the file, so it can be read and deleted
    if (SUCCEEDED(hr)) hr = SaveImage(Source{png}, flat);
    UINT w = 0, h = 0;
    const std::vector<BYTE> pixels = SUCCEEDED(hr) ? WicUpright(wic, flat, 1, w, h) : std::vector<BYTE>();
    int dark = 0;
    for (const BYTE b : pixels) dark += b < 250;
    if (pixels.empty() || dark) Fail("transparent pixels saved as JPEG aren't white", dark);
    for (const std::wstring& path : {jpg, copy, png, flat}) DeleteFileW(path.c_str());
}

// An image the decoded bitmap holds exactly, as a pasted one then stands in for its data, saves with every pixel
// from that bitmap on the 24bpp and palette paths, even while it is selected into a DC as the viewer keeps it.
// One with a color profile isn't exact, since the bitmap would lose the profile.
void TestBitmapSource(IWICImagingFactory* wic) {
    std::mt19937 rng(11);
    const std::wstring png = TempPath(L"bitmap.png"), saved = TempPath(L"bitmap saved.png");
    const struct {
        UINT colors;
        WORD bpp;
    } kinds[] = {{0, 24}, {200, 8}, {16, 4}, {2, 1}};
    const HDC dc = CreateCompatibleDC(nullptr);
    for (const auto& kind : kinds) {
        Image image;
        DIBSECTION ds{};
        if (FAILED(WriteImage(wic, png, GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 150, 97, 0, rng,
                              kind.colors ? 97 : 0, kind.colors))) {
            Fail("can't write a bitmap test image", kind.bpp);
        } else if (FAILED(DecodeImage(Source{png}, image)) || !image.exact ||
                   GetObjectW(image.bitmap, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != kind.bpp) {
            Fail("decoded bitmap isn't the smallest exact one", kind.bpp);
        } else {
            const HGDIOBJ old = SelectObject(dc, image.bitmap);
            const HRESULT hr = SaveImage(Source{.bitmap = image.bitmap}, saved);
            SelectObject(dc, old);
            UINT w = 0, h = 0, sw = 0, sh = 0;
            const std::vector<BYTE> want = WicUpright(wic, png, 1, w, h);
            if (FAILED(hr) || want.empty() || want != WicUpright(wic, saved, 1, sw, sh) || sw != w || sh != h) {
                Fail("saving from the decoded bitmap changed pixels", kind.bpp);
            }
        }
        if (image.bitmap) DeleteObject(image.bitmap);
    }
    DeleteDC(dc);

    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring icc = std::wstring(system) + L"\\spool\\drivers\\color\\sRGB Color Space Profile.icm";
    ComPtr<IWICColorContext> profile;
    Image image;
    if (FAILED(wic->CreateColorContext(&profile)) || FAILED(profile->InitializeFromFilename(icc.c_str())) ||
        FAILED(WriteImage(wic, png, GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 150, 97, 0, rng, 97, 16,
                          profile.Get())) ||
        FAILED(DecodeImage(Source{png}, image))) {
        Fail("can't decode an image with a color profile", 0);
    } else if (image.exact) {
        Fail("an image with a color profile counts as exact", 0);
    }
    if (image.bitmap) DeleteObject(image.bitmap);
    for (const std::wstring& path : {png, saved}) DeleteFileW(path.c_str());
}

// A clipboard DIB is accepted only if its header is one Windows writes and its colors and pixels fit the data.
void TestClipboardDib() {
    struct Dib {
        BITMAPINFOHEADER header;
        BYTE rest[1024];
    };
    const auto measure = [](const Dib& dib, SIZE_T size, UINT64& offset) {
        return MeasureDib(reinterpret_cast<const BYTE*>(&dib), size, offset);
    };
    UINT64 offset = 0;
    // 3x2 at 24bpp: rows of 9 bytes padded to 12.
    Dib dib{{sizeof(BITMAPINFOHEADER), 3, 2, 1, 24, BI_RGB}};
    const SIZE_T size24 = sizeof(BITMAPINFOHEADER) + 24;
    if (measure(dib, size24, offset) != size24 || offset != sizeof(BITMAPINFOHEADER)) Fail("valid 24bpp DIB refused", 0);
    if (measure(dib, size24 - 1, offset)) Fail("truncated DIB accepted", 0);
    dib.header.biHeight = -2;  // top-down
    if (measure(dib, size24, offset) != size24) Fail("top-down DIB refused", 0);

    // 8bpp: a full color table unless biClrUsed says fewer.
    dib.header.biBitCount = 8;
    const SIZE_T size8 = sizeof(BITMAPINFOHEADER) + 256 * 4 + 8;
    if (measure(dib, size8, offset) != size8 || offset != sizeof(BITMAPINFOHEADER) + 1024) Fail("8bpp DIB refused", 0);
    dib.header.biClrUsed = 2;
    if (measure(dib, sizeof(BITMAPINFOHEADER) + 8 + 8, offset) != sizeof(BITMAPINFOHEADER) + 16) {
        Fail("8bpp DIB with 2 colors refused", 0);
    }
    dib.header.biClrUsed = 257;
    if (measure(dib, sizeof(dib), offset)) Fail("DIB with more colors than its depth accepted", 257);

    // 32bpp bitfields: three masks follow a plain header.
    dib.header = {sizeof(BITMAPINFOHEADER), 3, 2, 1, 32, BI_BITFIELDS};
    if (measure(dib, sizeof(BITMAPINFOHEADER) + 12 + 24, offset) != sizeof(BITMAPINFOHEADER) + 36 ||
        offset != sizeof(BITMAPINFOHEADER) + 12) {
        Fail("32bpp bitfields DIB refused", 0);
    }

    // Compressed, odd depths and sizes, and unknown headers never reach a decoder.
    const struct {
        DWORD headerSize;
        LONG width, height;
        WORD bpp;
        DWORD compression;
    } bad[] = {{sizeof(BITMAPINFOHEADER), 3, 2, 8, BI_RLE8}, {sizeof(BITMAPINFOHEADER), 3, 2, 24, BI_JPEG},
               {sizeof(BITMAPINFOHEADER), 3, 2, 24, BI_PNG},  {sizeof(BITMAPINFOHEADER), 3, 2, 24, BI_BITFIELDS},
               {sizeof(BITMAPINFOHEADER), 3, 2, 2, BI_RGB},   {sizeof(BITMAPINFOHEADER), 0, 2, 24, BI_RGB},
               {sizeof(BITMAPINFOHEADER), -3, 2, 24, BI_RGB}, {sizeof(BITMAPINFOHEADER), 3, 0, 24, BI_RGB},
               {sizeof(BITMAPINFOHEADER), 0x7FFFFFFF, 0x7FFFFFFF, 32, BI_RGB},
               {sizeof(BITMAPINFOHEADER) + 1, 3, 2, 24, BI_RGB}, {12, 3, 2, 24, BI_RGB}};
    for (int i = 0; i < int(std::size(bad)); ++i) {
        dib.header = {bad[i].headerSize, bad[i].width, bad[i].height, 1, bad[i].bpp, bad[i].compression};
        if (measure(dib, sizeof(dib), offset)) Fail("bad clipboard DIB accepted", i);
    }
    dib.header = {sizeof(BITMAPINFOHEADER), 3, 2, 2, 24, BI_RGB};  // two planes
    if (measure(dib, sizeof(dib), offset)) Fail("DIB with two planes accepted", 0);
    if (measure(dib, sizeof(BITMAPINFOHEADER) - 1, offset)) Fail("DIB shorter than its header accepted", 0);
}

// The first file of a DROPFILES list, as Explorer copies or drops it, or none if the list is malformed.
void TestClipboardFiles() {
    const auto first = [](DWORD pFiles, BOOL wide, const wchar_t* names, size_t chars, std::wstring& path) {
        const SIZE_T size = sizeof(DROPFILES) + chars * sizeof(wchar_t);
        const HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, size);
        BYTE* const p = static_cast<BYTE*>(GlobalLock(data));
        const DROPFILES drop{pFiles, {}, FALSE, wide};
        memcpy(p, &drop, sizeof(drop));
        memcpy(p + sizeof(drop), names, chars * sizeof(wchar_t));
        GlobalUnlock(data);
        path.clear();
        const HRESULT hr = FirstFileOf(data, path);
        GlobalFree(data);
        return hr;
    };
    std::wstring path;
    const wchar_t names[] = L"C:\\a.png\0C:\\b.png\0";
    if (first(sizeof(DROPFILES), TRUE, names, std::size(names), path) != S_OK || path != L"C:\\a.png") {
        Fail("first dropped file not read", 0);
    }
    if (first(sizeof(DROPFILES), FALSE, names, std::size(names), path) != S_FALSE) Fail("ANSI file list accepted", 0);
    if (first(sizeof(DROPFILES) - 1, TRUE, names, std::size(names), path) != S_FALSE) {
        Fail("file list overlapping its header accepted", 0);
    }
    if (first(DWORD(sizeof(DROPFILES) + sizeof(names)), TRUE, names, std::size(names), path) != S_FALSE) {
        Fail("file list past its data accepted", 0);
    }
    if (first(sizeof(DROPFILES), TRUE, L"\0", 2, path) != S_FALSE) Fail("empty file name accepted", 0);
    if (FirstFileOf(nullptr, path) != S_FALSE) Fail("missing file list accepted", 0);
}

// The file name an <img src> URL gives a pasted image: decoded, but never a path, a hidden name, or one that
// reads differently than it is.
void TestClipboardName() {
    const struct {
        const char* url;
        const wchar_t* name;
    } cases[] = {
        {"https://example.com/images/cat.png", L"cat.png"},
        {"https://example.com/a%20b.JPG?size=large#top", L"a b.JPG"},
        {"https://example.com/caf%C3%A9.webp", L"caf\u00E9.webp"},
        {"cat.png", L"cat.png"},
        {"https://example.com/cat.txt", L""},
        {"https://example.com/", L""},
        {"https://example.com/.png", L""},
        {"https://example.com/a%2F..%2Fb.png", L""},
        {"https://example.com/a%5Cb.png", L""},
        {"https://example.com/a%3Ab.png", L""},
        {"https://example.com/a%0Ab.png", L""},
        {"https://example.com/a%E2%80%AEgnp.exe.png", L""},
        {"https://example.com/a%E2%81%A6b.png", L""},
        {"https://example.com/a%FFb.png", L""},
        {"https://example.com/a%zzb.png", L""},
        {"https://example.com/a%2", L""},
    };
    for (int i = 0; i < int(std::size(cases)); ++i) {
        if (ImageNameOf(cases[i].url) != cases[i].name) Fail("wrong name from an image URL", i);
    }
    const std::string longName = std::string(300, 'a') + ".png";
    if (!ImageNameOf(longName).empty()) Fail("name longer than a file name accepted", 0);
}

// Each frame is drawn over the last, after undoing it as its disposal asks: 2 clears to the background, 3
// restores what was there, and transparent pixels leave the canvas as it is.
void TestAnimationDisposal() {
    constexpr UINT kW = 4, kH = 2, kStride = 12;
    constexpr UINT32 kRed = 0xFF0000, kGreen = 0x00FF00, kBlue = 0x0000FF;
    BYTE canvas[kStride * kH];
    Animation a;
    a.width = kW;
    a.height = kH;
    a.stride = kStride;
    a.bits = canvas;
    const auto frame = [](RECT rect, BYTE disposal, int transparent, size_t offset) {
        Frame f{rect, 100, disposal, transparent, offset, {}};
        f.colors[1] = kRed;
        f.colors[2] = kGreen;
        f.colors[3] = kBlue;
        return f;
    };
    a.frames = {frame({0, 0, 4, 2}, 0, -1, 0), frame({1, 0, 3, 1}, 3, -1, 8), frame({0, 1, 1, 2}, 2, -1, 10),
                frame({3, 1, 4, 2}, 0, 0, 11)};
    a.pixels = AllocPages<BYTE>(12);
    const BYTE indices[12] = {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 0};
    memcpy(a.pixels.get(), indices, sizeof(indices));
    a.saved.resize(2 * 3);

    const auto at = [&](UINT x, UINT y) {
        const BYTE* p = canvas + y * kStride + x * 3;
        return UINT32(p[0] | p[1] << 8 | p[2] << 16);
    };
    const auto expect = [&](const UINT32 (&want)[kH][kW], int step) {
        for (UINT y = 0; y < kH; ++y) {
            for (UINT x = 0; x < kW; ++x) {
                if (at(x, y) != want[y][x]) return Fail("animation frame drawn wrong", step);
            }
        }
    };
    memset(canvas, 0x55, sizeof(canvas));
    ShowFrame(a, 0);
    expect({{kRed, kRed, kRed, kRed}, {kRed, kRed, kRed, kRed}}, 0);
    ShowFrame(a, 1);
    expect({{kRed, kGreen, kGreen, kRed}, {kRed, kRed, kRed, kRed}}, 1);
    ShowFrame(a, 2);  // restores under frame 1
    expect({{kRed, kRed, kRed, kRed}, {kBlue, kRed, kRed, kRed}}, 2);
    ShowFrame(a, 3);  // clears under frame 2, and draws nothing itself
    expect({{kRed, kRed, kRed, kRed}, {0, kRed, kRed, kRed}}, 3);
    ShowFrame(a, 0);  // a new loop starts over
    expect({{kRed, kRed, kRed, kRed}, {kRed, kRed, kRed, kRed}}, 4);
}

}  // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    {
        const WicFactory wic;
        if (FAILED(wic.hr)) return 1;
        TestScaling();
        TestOrientation(wic.Get());
        TestSave(wic.Get());
        TestBitmapSource(wic.Get());
    }
    TestClipboardDib();
    TestClipboardFiles();
    TestClipboardName();
    TestAnimationDisposal();
    CoUninitialize();
    printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
