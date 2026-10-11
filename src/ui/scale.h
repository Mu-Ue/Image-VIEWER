#pragma once

#include <windows.h>

// Draws the top-down DIB section selected into imageDC shrunk to width x height at (x, y), only where it
// meets clip. Each drawn pixel is the exact average of the image area it covers. Returns false if the
// bitmap is not a 24bpp or palettized DIB section, so the caller can fall back to StretchBlt.
bool DrawScaled(HDC dc, HDC imageDC, int x, int y, int width, int height, const RECT& clip);
