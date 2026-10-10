#pragma once

#include <string>

#include "imaging/decoder.h"

// Re-reads src at full size and quality and encodes it as PNG, or JPEG for a .jpg/.jpeg path.
HRESULT SaveImage(const Source& src, const std::wstring& path);
