#include "imaging/encoder.h"

using Microsoft::WRL::ComPtr;

// Writes a temporary file first so a failure never leaves a partial file and the source itself can
// be overwritten.
HRESULT SaveImage(const Source& src, const std::wstring& path) {
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
        if (SUCCEEDED(hr)) hr = OpenSource(wic.Get(), src, 0, source, width, height);
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
