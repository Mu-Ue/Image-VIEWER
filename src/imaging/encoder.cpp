#include "imaging/encoder.h"

#include <cstring>
#include <vector>

#include "platform/handle_stream.h"

using Microsoft::WRL::ComPtr;

namespace {

// Creates a new, empty file next to path to write into, never reusing an existing one.
HANDLE CreateTempFile(const std::wstring& path) {
    const std::wstring prefix = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".";
    for (UINT i = 0; i < 100; ++i) {
        const std::wstring temp = prefix + std::to_wstring(i) + L".tmp";
        const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) return file;
    }
    return INVALID_HANDLE_VALUE;
}

// Renames an open file to path, replacing any file there.
HRESULT RenameTo(HANDLE file, const std::wstring& path) {
    const DWORD bytes = DWORD(path.size() * sizeof(wchar_t));
    std::vector<BYTE> buffer(sizeof(FILE_RENAME_INFO) + bytes);
    auto* const info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = TRUE;
    info->FileNameLength = bytes;
    memcpy(info->FileName, path.data(), bytes);
    return SetFileInformationByHandle(file, FileRenameInfo, info, DWORD(buffer.size()))
               ? S_OK
               : HRESULT_FROM_WIN32(GetLastError());
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
        {L".tiff", GUID_ContainerFormatTiff},
    };
    const size_t dot = path.find_last_of(L".\\/");
    if (dot != std::wstring::npos && path[dot] == L'.') {
        for (const auto& type : kTypes) {
            if (CompareStringOrdinal(path.c_str() + dot, -1, type.ext, -1, TRUE) == CSTR_EQUAL) return type.container;
        }
    }
    return GUID_ContainerFormatPng;
}

// Writes the encoder options that keep the most quality: best-quality JPEG without chroma subsampling
// and BMP with alpha.
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

// Composites source over white, as other programs do for formats without alpha such as JPEG; just dropping
// the alpha would show whatever colors the transparent pixels hold.
HRESULT OverWhite(IWICImagingFactory* wic, IWICBitmapSource* source, ComPtr<IWICBitmapSource>& out) {
    ComPtr<IWICFormatConverter> premultiplied;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapLock> lock;
    HRESULT hr = wic->CreateFormatConverter(&premultiplied);
    if (SUCCEEDED(hr)) {
        hr = premultiplied->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapFromSource(premultiplied.Get(), WICBitmapCacheOnLoad, &bitmap);
    if (SUCCEEDED(hr)) hr = bitmap->Lock(nullptr, WICBitmapLockRead | WICBitmapLockWrite, &lock);
    UINT size = 0;
    BYTE* data = nullptr;
    if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&size, &data);
    if (FAILED(hr)) return hr;
    for (BYTE* p = data; p + 4 <= data + size; p += 4) {  // stride padding, if any, is harmless to change
        const BYTE white = BYTE(255 - p[3]);
        p[0] = BYTE(p[0] + white);
        p[1] = BYTE(p[1] + white);
        p[2] = BYTE(p[2] + white);
        p[3] = 255;
    }
    lock.Reset();
    out = bitmap;
    return S_OK;
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

// Encodes source into container, keeping its pixel format (bit depth, alpha, palette) and color profiles
// where the encoder supports them.
HRESULT Encode(IWICImagingFactory* wic, IWICBitmapSource* source, IStream* stream, const GUID& container,
               const std::vector<ComPtr<IWICColorContext>>& profiles = {}) {
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
    if (SUCCEEDED(hr) && !profiles.empty()) {  // without its profile, a wide-gamut image would be read as sRGB
        std::vector<IWICColorContext*> raw;
        for (const auto& p : profiles) raw.push_back(p.Get());
        frame->SetColorContexts(UINT(raw.size()), raw.data());  // formats without profiles fail, harmlessly
    }

    WICPixelFormatGUID sourceFormat{}, format{};
    if (SUCCEEDED(hr)) hr = source->GetPixelFormat(&sourceFormat);
    format = sourceFormat;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    ComPtr<IWICBitmapSource> pixels = source;
    if (SUCCEEDED(hr) && format == GUID_WICPixelFormat8bppIndexed && format != sourceFormat) {
        hr = ToIndexed(wic, source, HasAlpha(wic, sourceFormat), frame.Get(), pixels);
    } else if (SUCCEEDED(hr) && format != sourceFormat) {
        if (HasAlpha(wic, sourceFormat) && !HasAlpha(wic, format)) hr = OverWhite(wic, source, pixels);
        // Same as WICConvertBitmapSource, without linking windowscodecs.dll into every launch.
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(pixels.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
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

// Copies the rest of from into to.
HRESULT CopyStream(IStream* from, IStream* to) {
    std::vector<BYTE> buffer(1 << 16);
    for (;;) {
        ULONG n = 0;
        HRESULT hr = from->Read(buffer.data(), ULONG(buffer.size()), &n);
        if (FAILED(hr) || n == 0) return FAILED(hr) ? hr : S_OK;
        hr = to->Write(buffer.data(), n, nullptr);
        if (FAILED(hr)) return hr;
    }
}

}  // namespace

// Writes a temporary file first so a failure never leaves a partial file and the source itself can
// be overwritten. The file is written, renamed and deleted through one handle, so another program can't
// swap in a different file, or a link to one, along the way.
HRESULT SaveImage(const Source& src, const std::wstring& path) {
    const HANDLE file = CreateTempFile(path);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    ComPtr<IStream> stream;  // owns the file once made
    HRESULT hr = CreateHandleStream(file, stream);
    if (SUCCEEDED(hr)) {
        // Saving in the source's own format copies it byte for byte: re-encoding would lose quality,
        // animation and metadata.
        ComPtr<IStream> encoded;
        GUID format{};
        if (SUCCEEDED(OpenEncoded(src, encoded, format)) && format == ContainerFor(path)) {
            hr = CopyStream(encoded.Get(), stream.Get());
        } else {
            encoded.Reset();
            const WicFactory wic;
            hr = wic.hr;
            ComPtr<IWICBitmapSource> source;
            UINT width = 0, height = 0;
            std::vector<ComPtr<IWICColorContext>> profiles;
            if (SUCCEEDED(hr)) hr = OpenSource(wic.Get(), src, 0, source, width, height);
            if (SUCCEEDED(hr)) ReadColorProfiles(wic.Get(), src, profiles);
            if (SUCCEEDED(hr)) hr = Encode(wic.Get(), source.Get(), stream.Get(), ContainerFor(path), profiles);
        }
    }  // releases the source file, which may be the one replaced

    if (SUCCEEDED(hr)) hr = RenameTo(file, path);
    if (FAILED(hr)) {
        FILE_DISPOSITION_INFO remove{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
    }
    if (!stream) CloseHandle(file);
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
