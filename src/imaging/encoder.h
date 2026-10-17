#pragma once

#include <string>

#include "imaging/decoder.h"

// Copies src unchanged if path's extension (see kSaveFilter) names its own format, else re-reads it at full
// size and quality and encodes it in that format, or PNG.
HRESULT SaveImage(const Source& src, const std::wstring& path);

// Re-reads src at full size and quality as PNG (every pixel and alpha) and as a 24bpp DIB for the clipboard.
// Needs COM initialized on the calling thread.
HRESULT EncodeClipboardImage(const Source& src, HGLOBAL& png, HGLOBAL& dib);
