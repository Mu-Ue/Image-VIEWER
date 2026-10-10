#include "imaging/decoder.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#include "config.h"

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

// Opens a path only if it names a regular file, not a device or pipe such as \\.\CON.
HRESULT OpenFileStream(IWICImagingFactory* wic, const std::wstring& path, ComPtr<IStream>& out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    const bool regular = GetFileType(file) == FILE_TYPE_DISK;
    CloseHandle(file);
    if (!regular) return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;

    ComPtr<IWICStream> stream;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_READ);
    if (SUCCEEDED(hr)) out = stream;
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

}  // namespace

HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize, ComPtr<IWICBitmapSource>& source,
                   UINT& width, UINT& height) {
    ComPtr<IStream> stream = src.stream;
    HRESULT hr = stream ? S_OK : OpenFileStream(wic, src.path, stream);
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) hr = CreateDecoder(wic, stream.Get(), decoder);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = GetFrame(wic, decoder.Get(), frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width == 0 || height == 0) return WINCODEC_ERR_BADIMAGE;
    if (UINT64(width) * height > kMaxSourcePixels) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;

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
