#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <vector>

// Where an image comes from: a file, an in-memory copy of pasted data, or a decoded bitmap that holds
// the exact source pixels (not owned).
struct Source {
    std::wstring path;
    Microsoft::WRL::ComPtr<IStream> stream;
    HBITMAP bitmap = nullptr;
};

struct Image {
    HBITMAP bitmap = nullptr;  // 24bpp or palettized top-down DIB section, composited over kBackground
    UINT width = 0;
    UINT height = 0;
    UINT fullWidth = 0, fullHeight = 0;  // upright size of the source, which is larger if it was scaled down
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
// full, if given, is set to the stored size before scaling. If orientation is given, it is set to the
// EXIF orientation (1-8) instead of applying it, and width and height are those of the stored pixels.
HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize,
                   Microsoft::WRL::ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height,
                   SIZE* full = nullptr, int* orientation = nullptr);

// Opens the encoded data of a file or pasted source, rewound, and sets format to its container. Fails for a
// decoded bitmap or data in a format this viewer doesn't open.
HRESULT OpenEncoded(const Source& src, Microsoft::WRL::ComPtr<IStream>& stream, GUID& format);

// Adds the ICC profiles of src's image to out, if it has any.
void ReadColorProfiles(IWICImagingFactory* wic, const Source& src,
                       std::vector<Microsoft::WRL::ComPtr<IWICColorContext>>& out);

struct Animation;

// Decodes into the smallest exact DIB section. Needs COM initialized on the calling thread. If animated is
// given, it is set to whether the image is an animated GIF, and out is then its canvas showing the first
// frame, for DecodeAnimation to play. Gives up with E_ABORT once stop, if given, is set.
HRESULT DecodeImage(const Source& src, Image& out, bool* animated = nullptr, const std::atomic<bool>* stop = nullptr);

// Reads every frame of an animated GIF, which can take a while for a large one, so it gives up with E_ABORT
// once stop is set. Needs COM initialized on the calling thread.
HRESULT DecodeAnimation(const Source& src, Animation& out, const std::atomic<bool>& stop);

// Whether a file name ends in one of the image extensions this viewer opens.
bool IsImageName(const wchar_t* name);
