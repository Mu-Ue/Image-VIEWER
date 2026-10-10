#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

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
    bool exact = false;  // the bitmap holds every source pixel unchanged, so it can stand in for the source
};

// Opens the upright image: the chosen frame, scaled to fit maxSize (0 = no limit), EXIF orientation applied.
// scaled, if given, is set to whether it had to be scaled down. If orientation is given, it is set to the
// EXIF orientation (1-8) instead of applying it, and width and height are those of the stored pixels.
HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize,
                   Microsoft::WRL::ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height,
                   bool* scaled = nullptr, int* orientation = nullptr);

// Decodes into the smallest exact DIB section. Needs COM initialized on the calling thread.
HRESULT DecodeImage(const Source& src, Image& out);
