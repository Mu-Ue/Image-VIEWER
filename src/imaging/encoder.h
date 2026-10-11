#pragma once

#include <string>

#include "imaging/decoder.h"

// Re-reads src at full size and quality and encodes it in the format of path's extension (see kSaveFilter),
// or PNG.
HRESULT SaveImage(const Source& src, const std::wstring& path);

// Re-reads src at full size and quality as PNG (every pixel and alpha) and as a 24bpp DIB for the clipboard.
// Needs COM initialized on the calling thread.
HRESULT EncodeClipboardImage(const Source& src, HGLOBAL& png, HGLOBAL& dib);
