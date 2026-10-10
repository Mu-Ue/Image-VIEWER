#pragma once

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

// Copies the clipboard image into a stream: PNG if offered (lossless, keeps alpha), else the DIB
// as a BMP file. Returns S_FALSE if the clipboard holds no image.
HRESULT CopyClipboardImage(HWND owner, Microsoft::WRL::ComPtr<IStream>& out);
