#pragma once

#include <windows.h>
#include <wincodec.h>

#include <atomic>
#include <vector>

#include "platform/pages.h"

// One GIF frame: palette indices for its part of the canvas and how it is shown.
struct Frame {
    RECT rect;  // on the canvas, clipped to it
    UINT delay;  // ms
    BYTE disposal;  // 2 = clear to the background afterwards, 3 = restore what was there before
    int transparent;  // palette index drawn as see-through, or -1
    size_t offset;  // of the frame's indices in Animation::pixels, rect width per row
    UINT32 colors[256];  // 0x00RRGGBB
};

// An animated GIF, played onto a 24bpp top-down canvas.
struct Animation {
    std::vector<Frame> frames;  // empty if the image is still
    Pages<BYTE> pixels;
    std::vector<BYTE> saved;  // the canvas under a disposal 3 frame, 3 bytes per pixel
    size_t current = 0;
    UINT plays = 0, played = 0;  // times to play (0 for ever), and played to the end so far
    BYTE* bits = nullptr;  // the canvas, owned by the image's bitmap
    UINT width = 0, height = 0, stride = 0;
};

// Reads every frame of a GIF decoder with more than one, or only the first if firstOnly. Fails if it has
// fewer, or is too large to play, and gives up with E_ABORT once stop is set.
HRESULT ReadAnimation(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, Animation& out, bool firstOnly,
                      const std::atomic<bool>* stop = nullptr);

// Draws frame i over the previous one on the canvas and returns how long to show it.
UINT ShowFrame(Animation& animation, size_t i);
