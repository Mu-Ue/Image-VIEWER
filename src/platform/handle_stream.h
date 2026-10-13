#pragma once

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

// A stream on an open file, which it closes once released. Going through the handle instead of reopening
// the path means the file that was checked or created is the one read or written. On failure the caller
// keeps the handle.
HRESULT CreateHandleStream(HANDLE file, Microsoft::WRL::ComPtr<IStream>& out);
