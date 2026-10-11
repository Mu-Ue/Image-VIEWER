#pragma once

#include <windows.h>

#include "imaging/decoder.h"

// Reads the clipboard image into out: a stream of PNG if offered (lossless, keeps alpha), else the DIB
// as a BMP file, or else the path of the first file copied in Explorer. Returns S_FALSE if there is none.
HRESULT ReadClipboardImage(HWND owner, Source& out);

// Puts an image on the clipboard as PNG and DIB, taking ownership of both. Returns whether it is there.
bool SetClipboardImage(HWND owner, HGLOBAL png, HGLOBAL dib);
