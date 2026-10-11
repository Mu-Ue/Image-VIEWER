#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

// Where an image comes from: a file, an in-memory copy of pasted data, the bytes of a file read ahead
// (not owned), or a decoded bitmap that holds the exact source pixels (not owned).
struct Source {
    std::wstring path;
    Microsoft::WRL::ComPtr<IStream> stream;
    const BYTE* data = nullptr;
    DWORD size = 0;
    HBITMAP bitmap = nullptr;
};

struct Image {
    HBITMAP bitmap = nullptr;  // 24bpp or palettized top-down DIB section, composited over kBackground
    UINT width = 0;
    UINT height = 0;
    bool exact = false;  // the bitmap holds every source pixel unchanged, so it can stand in for the source
};

// The WIC factory, made straight from windowscodecs.dll: CoCreateInstance would first look the class up
// in the COM catalog, which loads clbcatq.dll for good. Unloads the DLL again when destroyed, as COM would,
// so every WIC object must be released first.
class WicFactory {
public:
    WicFactory();
    ~WicFactory();
    WicFactory(const WicFactory&) = delete;
    WicFactory& operator=(const WicFactory&) = delete;

    HRESULT hr = S_OK;
    IWICImagingFactory* Get() const { return factory_.Get(); }
    IWICImagingFactory* operator->() const { return factory_.Get(); }

private:
    HMODULE module_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
};

// Opens the upright image: the chosen frame, scaled to fit maxSize (0 = no limit), EXIF orientation applied.
// scaled, if given, is set to whether it had to be scaled down. If orientation is given, it is set to the
// EXIF orientation (1-8) instead of applying it, and width and height are those of the stored pixels.
HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize,
                   Microsoft::WRL::ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height,
                   bool* scaled = nullptr, int* orientation = nullptr);

// Decodes into the smallest exact DIB section. Needs COM initialized on the calling thread.
HRESULT DecodeImage(const Source& src, Image& out);
