#include "imaging/encoder.h"

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

// Encodes source as PNG, or best-quality JPEG, keeping its pixel format (bit depth, alpha, palette) where
// the encoder supports it.
HRESULT Encode(IWICImagingFactory* wic, IWICBitmapSource* source, IStream* stream, bool jpeg) {
    UINT width = 0, height = 0;
    HRESULT hr = source->GetSize(&width, &height);
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    const GUID& container = jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
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

    WICPixelFormatGUID sourceFormat{}, format{};
    if (SUCCEEDED(hr)) hr = source->GetPixelFormat(&sourceFormat);
    format = sourceFormat;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    ComPtr<IWICBitmapSource> pixels = source;
    if (SUCCEEDED(hr) && format != sourceFormat) {
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
    const size_t dot = path.find_last_of(L".\\/");
    const wchar_t* ext = dot != std::wstring::npos && path[dot] == L'.' ? path.c_str() + dot : L"";
    const bool jpeg = CompareStringOrdinal(ext, -1, L".jpg", -1, TRUE) == CSTR_EQUAL ||
                      CompareStringOrdinal(ext, -1, L".jpeg", -1, TRUE) == CSTR_EQUAL;
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
        if (SUCCEEDED(hr)) hr = Encode(wic.Get(), source.Get(), stream.Get(), jpeg);
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
    if (SUCCEEDED(hr)) hr = Encode(wic.Get(), source.Get(), stream.Get(), false);
    stream.Reset();
    if (SUCCEEDED(hr)) hr = EncodeDib(wic.Get(), source.Get(), dib);
    if (FAILED(hr)) {
        if (encoded) GlobalFree(encoded);
        return hr;
    }
    png = encoded;
    return S_OK;
}
