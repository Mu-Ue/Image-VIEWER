// Checks that what the viewer shows and saves is exact. Build with "cmake --build build --target tests" and
// run build\tests.exe; it prints each failure and exits with 1 if there was any.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "imaging/decoder.h"
#include "imaging/encoder.h"
#include "ui/scale.h"

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

// Writes an image of random pixels in the given container and pixel format, with an EXIF orientation.
HRESULT WriteImage(IWICImagingFactory* wic, const std::wstring& path, const GUID& container, WICPixelFormatGUID format,
                   UINT w, UINT h, int orientation, std::mt19937& rng) {
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
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w * bpp / 8; ++x) pixels[size_t(y) * stride + x] = BYTE(indexed ? rng() % 200 : rng());
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

// DecodeImage applies every EXIF orientation like WIC does, on its 24bpp and palette paths.
void TestOrientation(IWICImagingFactory* wic) {
    std::mt19937 rng(3);
    struct {
        const GUID& container;
        WICPixelFormatGUID format;
        const wchar_t* ext;
    } kinds[] = {{GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, L".jpg"},
                 {GUID_ContainerFormatTiff, GUID_WICPixelFormat8bppIndexed, L".tif"}};
    for (const auto& kind : kinds) {
        for (int orientation = 1; orientation <= 8; ++orientation) {
            const std::wstring path = TempPath((L"orientation" + std::to_wstring(orientation) + kind.ext).c_str());
            // Wider than the 64-pixel tiles sideways images are written in, and odd-sized.
            if (FAILED(WriteImage(wic, path, kind.container, kind.format, 150, 97, orientation, rng))) {
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

}  // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    {
        const WicFactory wic;
        if (FAILED(wic.hr)) return 1;
        TestScaling();
        TestOrientation(wic.Get());
        TestSave(wic.Get());
    }
    CoUninitialize();
    printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
