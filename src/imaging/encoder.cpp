#include "imaging/encoder.h"

#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Creates a new, empty file next to path to write into, never reusing an existing one.
std::wstring CreateTempFile(const std::wstring& path) {
    const std::wstring prefix = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".";
    for (UINT i = 0; i < 100; ++i) {
        std::wstring temp = prefix + std::to_wstring(i) + L".tmp";
        const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return temp;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    return {};
}

// The container for path's extension, matching kSaveFilter; PNG for anything else.
const GUID& ContainerFor(const std::wstring& path) {
    static const struct {
        const wchar_t* ext;
        const GUID& container;
    } kTypes[] = {
        {L".jpg", GUID_ContainerFormatJpeg}, {L".jpeg", GUID_ContainerFormatJpeg},
        {L".jpe", GUID_ContainerFormatJpeg}, {L".jfif", GUID_ContainerFormatJpeg},
        {L".bmp", GUID_ContainerFormatBmp},  {L".dib", GUID_ContainerFormatBmp},
        {L".gif", GUID_ContainerFormatGif},  {L".tif", GUID_ContainerFormatTiff},
        {L".tiff", GUID_ContainerFormatTiff}, {L".jxr", GUID_ContainerFormatWmp},
        {L".wdp", GUID_ContainerFormatWmp},
    };
    const size_t dot = path.find_last_of(L".\\/");
    if (dot != std::wstring::npos && path[dot] == L'.') {
        for (const auto& type : kTypes) {
            if (CompareStringOrdinal(path.c_str() + dot, -1, type.ext, -1, TRUE) == CSTR_EQUAL) return type.container;
        }
    }
    return GUID_ContainerFormatPng;
}

// Writes the encoder options that keep the most quality: best-quality JPEG without chroma subsampling,
// lossless JPEG XR, and BMP with alpha.
HRESULT WriteOptions(IPropertyBag2* options, const GUID& container) {
    PROPBAG2 names[2] = {};
    VARIANT values[2] = {};
    ULONG count = 0;
    if (container == GUID_ContainerFormatJpeg) {
        names[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        values[0].vt = VT_R4;
        values[0].fltVal = 1.0f;
        names[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
        values[1].vt = VT_UI1;
        values[1].bVal = WICJpegYCrCbSubsampling444;
        count = 2;
    } else if (container == GUID_ContainerFormatWmp) {
        names[0].pstrName = const_cast<LPOLESTR>(L"Lossless");
        values[0].vt = VT_BOOL;
        values[0].boolVal = VARIANT_TRUE;
        count = 1;
    } else if (container == GUID_ContainerFormatBmp) {
        names[0].pstrName = const_cast<LPOLESTR>(L"EnableV5Header32bppBGRA");
        values[0].vt = VT_BOOL;
        values[0].boolVal = VARIANT_TRUE;
        count = 1;
    }
    return count ? options->Write(count, names, values) : S_OK;
}

// Whether pixels in format can be transparent.
bool HasAlpha(IWICImagingFactory* wic, const WICPixelFormatGUID& format) {
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo2> pixel;
    BOOL alpha = FALSE;
    return SUCCEEDED(wic->CreateComponentInfo(format, &info)) && SUCCEEDED(info.As(&pixel)) &&
           SUCCEEDED(pixel->SupportsTransparency(&alpha)) && alpha;
}

// Maps source onto its best 256 colors for an 8bpp indexed frame (GIF). The converter matches colors only,
// so with alpha it gets 255 colors, and pixels under half opacity become a transparent 256th.
HRESULT ToIndexed(IWICImagingFactory* wic, IWICBitmapSource* source, bool alpha, IWICBitmapFrameEncode* frame,
                  ComPtr<IWICBitmapSource>& out) {
    ComPtr<IWICPalette> palette;
    ComPtr<IWICFormatConverter> indexed;
    HRESULT hr = wic->CreatePalette(&palette);
    if (SUCCEEDED(hr)) hr = palette->InitializeFromBitmap(source, alpha ? 255 : 256, FALSE);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&indexed);
    if (SUCCEEDED(hr)) {
        hr = indexed->Initialize(source, GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeNone, palette.Get(),
                                 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr) || !alpha) {
        if (SUCCEEDED(hr)) hr = frame->SetPalette(palette.Get());
        out = indexed;
        return hr;
    }

    UINT width = 0, height = 0;
    hr = source->GetSize(&width, &height);
    if (SUCCEEDED(hr) && width > UINT_MAX / 4) hr = WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    WICColor colors[256];
    UINT count = 0;
    if (SUCCEEDED(hr)) hr = palette->GetColors(255, colors, &count);
    if (SUCCEEDED(hr)) {
        colors[count] = 0;
        hr = palette->InitializeCustom(colors, count + 1);
    }
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> bgra;
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapFromSource(indexed.Get(), WICBitmapCacheOnLoad, &bitmap);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&bgra);
    if (SUCCEEDED(hr)) {
        hr = bgra->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
    }
    ComPtr<IWICBitmapLock> lock;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (SUCCEEDED(hr)) hr = bitmap->Lock(nullptr, WICBitmapLockWrite, &lock);
    if (SUCCEEDED(hr)) hr = lock->GetStride(&stride);
    if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&size, &data);
    std::vector<BYTE> row;
    if (SUCCEEDED(hr)) row.resize(size_t(width) * 4);
    for (UINT y = 0; y < height && SUCCEEDED(hr); ++y) {
        const WICRect rect{0, INT(y), INT(width), 1};
        hr = bgra->CopyPixels(&rect, UINT(row.size()), UINT(row.size()), row.data());
        BYTE* const indices = data + size_t(y) * stride;
        for (UINT x = 0; x < width; ++x) {
            if (row[size_t(x) * 4 + 3] < 128) indices[x] = BYTE(count);
        }
    }
    lock.Reset();
    if (SUCCEEDED(hr)) hr = bitmap->SetPalette(palette.Get());
    if (SUCCEEDED(hr)) hr = frame->SetPalette(palette.Get());
    out = bitmap;
    return hr;
}

// Encodes source into container, keeping its pixel format (bit depth, alpha, palette) where the encoder
// supports it.
HRESULT Encode(IWICImagingFactory* wic, IWICBitmapSource* source, IStream* stream, const GUID& container) {
    UINT width = 0, height = 0;
    HRESULT hr = source->GetSize(&width, &height);
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr)) hr = WriteOptions(options.Get(), container);
    if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);

    WICPixelFormatGUID sourceFormat{}, format{};
    if (SUCCEEDED(hr)) hr = source->GetPixelFormat(&sourceFormat);
    format = sourceFormat;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    ComPtr<IWICBitmapSource> pixels = source;
    if (SUCCEEDED(hr) && format == GUID_WICPixelFormat8bppIndexed && format != sourceFormat) {
        hr = ToIndexed(wic, source, HasAlpha(wic, sourceFormat), frame.Get(), pixels);
    } else if (SUCCEEDED(hr) && format != sourceFormat) {
        // Same as WICConvertBitmapSource, without linking windowscodecs.dll into every launch.
        ComPtr<IWICFormatConverter> converter;
        hr = wic->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(source, format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        }
        pixels = converter;
    } else if (SUCCEEDED(hr)) {
        ComPtr<IWICPalette> palette;
        if (SUCCEEDED(wic->CreatePalette(&palette)) && SUCCEEDED(source->CopyPalette(palette.Get()))) {
            hr = frame->SetPalette(palette.Get());
        }
    }
    if (SUCCEEDED(hr)) hr = frame->WriteSource(pixels.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

// A plain bottom-up 24bpp DIB, the format every program reads, of source without its alpha.
HRESULT EncodeDib(IWICImagingFactory* wic, IWICBitmapSource* source, HGLOBAL& out) {
    UINT width = 0, height = 0;
    HRESULT hr = source->GetSize(&width, &height);
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) return hr;
    const UINT64 stride = (UINT64(width) * 3 + 3) & ~UINT64(3);
    const UINT64 bytes = stride * height;
    if (sizeof(BITMAPINFOHEADER) + bytes > MAXDWORD) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    const HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, SIZE_T(sizeof(BITMAPINFOHEADER) + bytes));
    BYTE* p = dib ? static_cast<BYTE*>(GlobalLock(dib)) : nullptr;
    if (!p) {
        if (dib) GlobalFree(dib);
        return E_OUTOFMEMORY;
    }
    const BITMAPINFOHEADER info{sizeof(info), LONG(width), LONG(height), 1, 24, BI_RGB, DWORD(bytes)};
    memcpy(p, &info, sizeof(info));
    // Rows are read top-down, the order decoders produce them in, and stored bottom-up.
    BYTE* const pixels = p + sizeof(info);
    for (UINT y = 0; y < height && SUCCEEDED(hr); ++y) {
        const WICRect row{0, INT(y), INT(width), 1};
        hr = converter->CopyPixels(&row, UINT(stride), UINT(stride), pixels + (height - 1 - y) * stride);
    }
    GlobalUnlock(dib);
    if (FAILED(hr)) {
        GlobalFree(dib);
        return hr;
    }
    out = dib;
    return S_OK;
}

}  // namespace

// Writes a temporary file first so a failure never leaves a partial file and the source itself can
// be overwritten.
HRESULT SaveImage(const Source& src, const std::wstring& path) {
    const std::wstring temp = CreateTempFile(path);
    if (temp.empty()) return HRESULT_FROM_WIN32(GetLastError());

    HRESULT hr;
    {
        const WicFactory wic;
        hr = wic.hr;
        ComPtr<IWICBitmapSource> source;
        UINT width = 0, height = 0;
        if (SUCCEEDED(hr)) hr = OpenSource(wic.Get(), src, 0, source, width, height);
        ComPtr<IWICStream> stream;
        if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = Encode(wic.Get(), source.Get(), stream.Get(), ContainerFor(path));
    }  // closes the temporary file

    if (SUCCEEDED(hr) && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(hr)) DeleteFileW(temp.c_str());
    return hr;
}

HRESULT EncodeClipboardImage(const Source& src, HGLOBAL& png, HGLOBAL& dib) {
    png = dib = nullptr;
    const WicFactory wic;
    HRESULT hr = wic.hr;
    ComPtr<IWICBitmapSource> source;
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = OpenSource(wic.Get(), src, 0, source, width, height);
    ComPtr<IStream> stream;
    HGLOBAL encoded = nullptr;
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, FALSE, &stream);
    if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream.Get(), &encoded);
    if (SUCCEEDED(hr)) hr = Encode(wic.Get(), source.Get(), stream.Get(), GUID_ContainerFormatPng);
    stream.Reset();
    if (SUCCEEDED(hr)) hr = EncodeDib(wic.Get(), source.Get(), dib);
    if (FAILED(hr)) {
        if (encoded) GlobalFree(encoded);
        return hr;
    }
    png = encoded;
    return S_OK;
}
