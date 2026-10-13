#include "imaging/decoder.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

#include "config.h"
#include "imaging/animation.h"
#include "platform/handle_stream.h"
#include "platform/pages.h"

using Microsoft::WRL::ComPtr;

namespace {

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

// Whether any pixel of the source can be less than fully opaque.
bool CanBeTransparent(IWICImagingFactory* wic, IWICBitmapSource* source) {
    WICPixelFormatGUID format;
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo2> formatInfo;
    BOOL transparency = TRUE;
    if (FAILED(source->GetPixelFormat(&format)) || FAILED(wic->CreateComponentInfo(format, &info)) ||
        FAILED(info.As(&formatInfo)) || FAILED(formatInfo->SupportsTransparency(&transparency))) {
        return true;
    }
    if (!transparency) return false;
    // Indexed formats are transparent only if their palette is.
    ComPtr<IWICPalette> palette;
    BOOL alpha = TRUE;
    if (SUCCEEDED(wic->CreatePalette(&palette)) && SUCCEEDED(source->CopyPalette(palette.Get())) &&
        SUCCEEDED(palette->HasAlpha(&alpha))) {
        return alpha;
    }
    return true;
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
    UINT32 keys[4096] = {};  // color | kUsed, 0 = empty slot
    BYTE indices[4096];

    // Returns the color's index, adding it if new, or -1 once the palette is full.
    int Find(UINT32 color) {
        // Folding the high bits in first keeps palettes built from regular color steps from clustering.
        UINT i = ((color ^ color >> 15) * 0x9E3779B1u) >> 20;
        for (; keys[i] != 0; i = (i + 1) & 4095) {
            if (keys[i] == (color | kUsed)) return indices[i];
        }
        if (count == 256) return -1;
        keys[i] = color | kUsed;
        indices[i] = BYTE(count);
        colors[count] = color;
        return int(count++);
    }
};

// Packs a row of palette indices at Bpp bits each, leftmost pixel in the high bits.
template <UINT Bpp>
void PackRow(const BYTE* in, UINT width, BYTE* d) {
    constexpr UINT perByte = 8 / Bpp;
    UINT x = 0;
    for (; x + perByte <= width; x += perByte) {
        BYTE packed = 0;
        for (UINT k = 0; k < perByte; ++k) packed = BYTE(packed << Bpp | in[x + k]);
        *d++ = packed;
    }
    if (x < width) {
        BYTE packed = 0;
        for (UINT k = 0; k < perByte; ++k) packed = BYTE(packed << Bpp | (x + k < width ? in[x + k] : 0));
        *d = packed;
    }
}

// Where source pixel (x, y) lands in an upright buffer, in bytes: origin + x * stepX + y * stepY.
struct Placement {
    ptrdiff_t origin, stepX, stepY;
};

// For a width x height source with the given EXIF orientation, stored upright with pixel bytes per
// pixel and stride bytes per upright row.
Placement Place(int orientation, UINT width, UINT height, ptrdiff_t pixel, ptrdiff_t stride) {
    const ptrdiff_t right = ptrdiff_t(width) - 1, bottom = ptrdiff_t(height) - 1;
    switch (orientation) {
        case 2: return {right * pixel, -pixel, stride};                    // mirrored
        case 3: return {bottom * stride + right * pixel, -pixel, -stride};  // rotated 180
        case 4: return {bottom * stride, pixel, -stride};                   // flipped
        case 5: return {0, stride, pixel};                                  // transposed
        case 6: return {bottom * pixel, stride, -pixel};                    // rotated 90 clockwise
        case 7: return {right * stride + bottom * pixel, -stride, -pixel};  // transversed
        case 8: return {right * stride, -stride, pixel};                    // rotated 270 clockwise
        default: return {0, pixel, stride};
    }
}

// The upright rectangle that source rows [0, rows) land in.
RECT PlacedRows(int orientation, UINT width, UINT height, UINT rows) {
    const LONG w = LONG(width), h = LONG(height), n = LONG(rows);  // source size
    switch (orientation) {
        case 1: case 2: return {0, 0, w, n};
        case 3: case 4: return {0, h - n, w, h};
        case 5: case 8: return {0, 0, n, w};
        default: return {h - n, 0, h, w};
    }
}

// Calls visit(i, offset) for the source pixels of rows [y0, y1): i counts pixels from (0, y0) in row
// order, offset is where the pixel lands. Stops early if visit returns false. Sideways images are
// walked in column tiles, so their writes, which run down upright columns, stay in cache.
template <ptrdiff_t Pixel, typename Visit>
bool ForEachPixel(UINT width, UINT y0, UINT y1, const Placement& place, bool sideways, Visit&& visit) {
    const auto walk = [&](auto stepX) {
        const UINT tile = sideways ? 64 : width;
        for (UINT x0 = 0; x0 < width; x0 += tile) {
            const UINT x1 = std::min(width, x0 + tile);
            for (UINT y = y0; y < y1; ++y) {
                size_t i = size_t(y - y0) * width + x0;
                ptrdiff_t offset = place.origin + ptrdiff_t(x0) * stepX + ptrdiff_t(y) * place.stepY;
                for (UINT x = x0; x < x1; ++x, ++i, offset += stepX) {
                    if (!visit(i, offset)) return false;
                }
            }
        }
        return true;
    };
    // Upright rows, the common case, get a constant step the compiler can optimize for.
    return place.stepX == Pixel ? walk(std::integral_constant<ptrdiff_t, Pixel>{}) : walk(place.stepX);
}

// Whether converting the format to 8-bit BGRA keeps every pixel's value.
bool Is8BitColor(const WICPixelFormatGUID& format) {
    for (const GUID& f : {GUID_WICPixelFormat24bppBGR, GUID_WICPixelFormat32bppBGR, GUID_WICPixelFormat32bppBGRA,
                          GUID_WICPixelFormat8bppIndexed, GUID_WICPixelFormat4bppIndexed,
                          GUID_WICPixelFormat2bppIndexed, GUID_WICPixelFormat1bppIndexed}) {
        if (format == f) return true;
    }
    return false;
}

// The formats this viewer opens, recognized by their first bytes rather than the file name.
const GUID* SniffFormat(const BYTE* head, ULONG size) {
    const auto at = [&](ULONG offset, const char* magic, ULONG length) {
        return size >= offset + length && memcmp(head + offset, magic, length) == 0;
    };
    if (at(0, "\xFF\xD8\xFF", 3)) return &GUID_ContainerFormatJpeg;
    if (at(0, "\x89PNG\r\n\x1A\n", 8)) return &GUID_ContainerFormatPng;
    if (at(0, "GIF87a", 6) || at(0, "GIF89a", 6)) return &GUID_ContainerFormatGif;
    if (at(0, "BM", 2)) return &GUID_ContainerFormatBmp;
    if (at(0, "II*\0", 4) || at(0, "MM\0*", 4)) return &GUID_ContainerFormatTiff;
    if (at(0, "II\xBC", 3)) return &GUID_ContainerFormatWmp;  // JPEG XR
    if (at(0, "\0\0\1\0", 4)) return &GUID_ContainerFormatIco;
    if (at(0, "RIFF", 4) && at(8, "WEBP", 4)) return &GUID_ContainerFormatWebp;
    if (at(4, "ftyp", 4)) {  // HEIC and AVIF, both read by the HEIF decoder
        for (const char* brand : {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1", "avif", "avis"}) {
            if (at(8, brand, 4)) return &GUID_ContainerFormatHeif;
        }
    }
    return nullptr;
}

// Opens a path only if it names a regular file, not a device or pipe such as \\.\CON, and reads that same
// file through the handle checked. A pipe server can only identify the user, never act as them.
HRESULT OpenFileStream(const std::wstring& path, ComPtr<IStream>& out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    const HRESULT hr =
        GetFileType(file) == FILE_TYPE_DISK ? CreateHandleStream(file, out) : WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
    if (FAILED(hr)) CloseHandle(file);
    return hr;
}

// Creates the decoder for the format the data starts with, and only that one: data is never handed
// to a codec for a different format than it claims to be, and anything else is rejected.
HRESULT CreateDecoder(IWICImagingFactory* wic, IStream* stream, ComPtr<IWICBitmapDecoder>& out) {
    BYTE head[16];
    ULONG read = 0;
    HRESULT hr = stream->Seek({}, STREAM_SEEK_SET, nullptr);
    if (SUCCEEDED(hr)) hr = stream->Read(head, sizeof(head), &read);
    if (SUCCEEDED(hr)) hr = stream->Seek({}, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;
    const GUID* format = SniffFormat(head, read);
    if (!format) return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = wic->CreateDecoder(*format, &GUID_VendorMicrosoft, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->Initialize(stream, WICDecodeMetadataCacheOnDemand);
    if (SUCCEEDED(hr)) out = decoder;
    return hr;
}

HRESULT OpenDecoder(IWICImagingFactory* wic, const Source& src, ComPtr<IWICBitmapDecoder>& decoder) {
    ComPtr<IStream> stream = src.stream;
    HRESULT hr = stream ? S_OK : OpenFileStream(src.path, stream);
    if (SUCCEEDED(hr)) hr = CreateDecoder(wic, stream.Get(), decoder);
    return hr;
}

// Reads an animated GIF and shows its first frame on a new 24bpp canvas. Fails for anything else.
HRESULT DecodeAnimation(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, Image& out, Animation& animation) {
    GUID container;
    HRESULT hr = decoder->GetContainerFormat(&container);
    if (SUCCEEDED(hr) && container != GUID_ContainerFormatGif) hr = WINCODEC_ERR_UNSUPPORTEDOPERATION;
    Animation a;
    if (SUCCEEDED(hr)) hr = ReadAnimation(wic, decoder, a);
    if (FAILED(hr)) return hr;
    const HBITMAP bitmap = CreateDib(a.width, a.height, 24, nullptr, 0, a.bits);
    if (!bitmap) return E_OUTOFMEMORY;
    a.stride = (a.width * 3 + 3) & ~3u;
    ShowFrame(a, 0);
    out.bitmap = bitmap;
    out.width = a.width;
    out.height = a.height;
    out.exact = false;  // the canvas changes as it plays
    animation = std::move(a);
    return S_OK;
}

}  // namespace

WicFactory::WicFactory() {
    module_ = LoadLibraryExW(L"windowscodecs.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto getClassObject =
        module_ ? reinterpret_cast<decltype(&DllGetClassObject)>(GetProcAddress(module_, "DllGetClassObject")) : nullptr;
    if (!getClassObject) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        return;
    }
    ComPtr<IClassFactory> classFactory;
    hr = getClassObject(CLSID_WICImagingFactory, IID_PPV_ARGS(&classFactory));
    if (SUCCEEDED(hr)) hr = classFactory->CreateInstance(nullptr, IID_PPV_ARGS(&factory_));
}

WicFactory::~WicFactory() {
    factory_.Reset();
    if (module_) FreeLibrary(module_);
}

namespace {

// OpenSource for a source that isn't a bitmap, once its decoder is made.
HRESULT OpenDecoded(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, UINT maxSize,
                    ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height, bool* scaled, int* orientation) {
    if (scaled) *scaled = false;
    if (orientation) *orientation = 1;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = GetFrame(wic, decoder, frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width == 0 || height == 0) return WINCODEC_ERR_BADIMAGE;
    if (UINT64(width) * height > kMaxSourcePixels) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;

    source = frame;
    const bool scale = maxSize && (width > maxSize || height > maxSize);
    if (scaled) *scaled = scale;
    if (scale) {
        // Scaling at the decoder lets codecs like JPEG decode at reduced size directly.
        const double factor = double(maxSize) / std::max(width, height);
        width = std::max(1u, UINT(width * factor));
        height = std::max(1u, UINT(height * factor));
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
    const int exif = ReadOrientation(frame.Get());
    if (orientation) {
        *orientation = exif;
    } else if (exif != 1) {
        // The rotator reads its source pixel by pixel, so buffer it to keep strip reads below linear.
        ComPtr<IWICBitmap> buffered;
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = wic->CreateBitmapFromSource(source.Get(), WICBitmapCacheOnLoad, &buffered);
        if (SUCCEEDED(hr)) hr = wic->CreateBitmapFlipRotator(&rotator);
        if (SUCCEEDED(hr)) hr = rotator->Initialize(buffered.Get(), kTransforms[exif]);
        if (FAILED(hr)) return hr;
        if (exif >= 5) std::swap(width, height);
        source = rotator;
    }
    return S_OK;
}

}  // namespace

HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize, ComPtr<IWICBitmapSource>& source,
                   UINT& width, UINT& height, bool* scaled, int* orientation) {
    if (src.bitmap) {
        if (scaled) *scaled = false;
        if (orientation) *orientation = 1;
        ComPtr<IWICBitmap> bitmap;
        HRESULT hr = wic->CreateBitmapFromHBITMAP(src.bitmap, nullptr, WICBitmapIgnoreAlpha, &bitmap);
        if (SUCCEEDED(hr)) hr = bitmap->GetSize(&width, &height);
        if (SUCCEEDED(hr)) source = bitmap;
        return hr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = OpenDecoder(wic, src, decoder);
    if (SUCCEEDED(hr)) hr = OpenDecoded(wic, decoder.Get(), maxSize, source, width, height, scaled, orientation);
    return hr;
}

HRESULT DecodeImage(const Source& src, Image& out, Animation* animation) {
    const WicFactory wic;
    HRESULT hr = wic.hr;
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapSource> source;
    UINT width = 0, height = 0;  // of the source, before orientation is applied
    bool scaled = false;
    int orientation = 1;
    if (src.bitmap) {
        hr = OpenSource(wic.Get(), src, kMaxBitmapSize, source, width, height, &scaled, &orientation);
    } else {
        ComPtr<IWICBitmapDecoder> decoder;
        hr = OpenDecoder(wic.Get(), src, decoder);
        if (SUCCEEDED(hr) && animation && SUCCEEDED(DecodeAnimation(wic.Get(), decoder.Get(), out, *animation))) {
            return S_OK;
        }
        if (SUCCEEDED(hr)) {
            hr = OpenDecoded(wic.Get(), decoder.Get(), kMaxBitmapSize, source, width, height, &scaled, &orientation);
        }
    }
    if (FAILED(hr)) return hr;
    WICPixelFormatGUID format{};
    source->GetPixelFormat(&format);
    const bool transparent = CanBeTransparent(wic.Get(), source.Get());

    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) return hr;

    // Decode in strips through a small 32bpp buffer and store the exact pixels upright, in the smallest
    // form: palette indices while the image has <= 256 colors, otherwise 24bpp (alpha is composited away).
    const bool sideways = orientation >= 5;
    const UINT outW = sideways ? height : width, outH = sideways ? width : height;
    const UINT stripStride = width * 4;
    const UINT rows = std::clamp((1u << 20) / stripStride, 1u, height);
    const Pages<BYTE> strip = AllocPages<BYTE>(size_t(stripStride) * rows);
    Pages<BYTE> indices = AllocPages<BYTE>(size_t(width) * height);
    if (!strip) return E_OUTOFMEMORY;

    const UINT stride24 = (outW * 3 + 3) & ~3u;
    const Placement placeIndex = Place(orientation, width, height, 1, outW);
    const Placement place24 = Place(orientation, width, height, 3, stride24);
    Palette palette;
    UINT32 lastColor = ~0u;
    BYTE lastIndex = 0;
    BYTE* bits = nullptr;
    HBITMAP bitmap = indices ? nullptr : CreateDib(outW, outH, 24, nullptr, 0, bits);
    if (!indices && !bitmap) return E_OUTOFMEMORY;

    // Once 24bpp, upright opaque 8-bit sources are decoded straight into the bitmap: same bytes, no copy.
    const bool canDecodeDirect = orientation == 1 && !transparent && Is8BitColor(format);
    ComPtr<IWICFormatConverter> direct;
    const UINT bg = kBackground & 0xFF;  // gray: all channels equal
    BYTE alpha = 255;  // AND of all alpha values: 255 if nothing is transparent
    for (UINT y0 = 0; y0 < height; y0 += rows) {
        const UINT n = std::min(rows, height - y0);
        const WICRect rect{0, INT(y0), INT(width), INT(n)};
        if (direct) {
            hr = direct->CopyPixels(&rect, stride24, stride24 * n, bits + size_t(y0) * stride24);
            if (FAILED(hr)) break;
            continue;
        }
        hr = converter->CopyPixels(&rect, stripStride, stripStride * n, strip.get());
        if (FAILED(hr)) break;
        BYTE* const begin = strip.get();
        BYTE* const end = begin + size_t(stripStride) * n;

        // Composite transparent pixels over the background once, so painting is a plain blit; sources
        // that cannot be transparent skip this.
        for (BYTE* p = transparent ? begin : end; p != end; p += 4) {
            alpha &= p[3];
            const UINT inverse = 255u - p[3];
            if (inverse == 0) continue;
            const BYTE add = BYTE((bg * inverse + 127) / 255);
            p[0] = BYTE(p[0] + add);
            p[1] = BYTE(p[1] + add);
            p[2] = BYTE(p[2] + add);
        }

        if (indices) {
            BYTE* const dst = indices.get();
            const auto store = [&](size_t i, ptrdiff_t at) {
                UINT32 color;
                memcpy(&color, begin + i * 4, 4);
                color &= 0xFFFFFF;
                if (color != lastColor) {
                    const int index = palette.Find(color);
                    if (index < 0) return false;
                    lastColor = color;
                    lastIndex = BYTE(index);
                }
                dst[at] = lastIndex;
                return true;
            };
            if (ForEachPixel<1>(width, y0, y0 + n, placeIndex, sideways, store)) continue;

            // More than 256 colors: switch to 24bpp, expanding the rows stored so far.
            bitmap = CreateDib(outW, outH, 24, nullptr, 0, bits);
            if (!bitmap) {
                hr = E_OUTOFMEMORY;
                break;
            }
            const RECT done = PlacedRows(orientation, width, height, y0);
            for (LONG y = done.top; y < done.bottom; ++y) {
                const BYTE* in = indices.get() + size_t(y) * outW;
                BYTE* d = bits + size_t(y) * stride24 + size_t(done.left) * 3;
                for (LONG x = done.left; x < done.right; ++x, d += 3) {
                    const UINT32 color = palette.colors[in[x]];
                    d[0] = BYTE(color);
                    d[1] = BYTE(color >> 8);
                    d[2] = BYTE(color >> 16);
                }
            }
            indices.reset();
        }

        ForEachPixel<3>(width, y0, y0 + n, place24, sideways, [&](size_t i, ptrdiff_t at) {
            const BYTE* p = begin + i * 4;
            BYTE* d = bits + at;
            d[0] = p[0];
            d[1] = p[1];
            d[2] = p[2];
            return true;
        });
        if (canDecodeDirect &&
            SUCCEEDED(wic->CreateFormatConverter(&direct)) &&
            FAILED(direct->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                      nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            direct.Reset();
        }
    }
    if (FAILED(hr)) {
        if (bitmap) DeleteObject(bitmap);
        return hr;
    }

    if (indices) {
        const WORD bpp = palette.count <= 2 ? 1 : palette.count <= 16 ? 4 : 8;
        bitmap = CreateDib(outW, outH, bpp, palette.colors, palette.count, bits);
        if (!bitmap) return E_OUTOFMEMORY;
        const UINT stride = (outW * bpp + 31) / 32 * 4;
        for (UINT y = 0; y < outH; ++y) {
            const BYTE* in = indices.get() + size_t(y) * outW;
            BYTE* d = bits + size_t(y) * stride;
            if (bpp == 8) std::copy_n(in, outW, d);
            else if (bpp == 4) PackRow<4>(in, outW, d);
            else PackRow<1>(in, outW, d);
        }
    }

    out.bitmap = bitmap;
    out.width = outW;
    out.height = outH;
    out.exact = alpha == 255 && !scaled && Is8BitColor(format);
    return S_OK;
}

bool IsImageName(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    for (const wchar_t* ext : kExtensions) {
        if (CompareStringOrdinal(dot, -1, ext, -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}
