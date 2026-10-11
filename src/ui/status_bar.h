#pragma once

#include <windows.h>

#include <string>

// Creates the dark bar across the bottom of parent's client area, sized for parent's DPI. It shows the image
// info and the previous/next buttons, which arrive at parent as WM_COMMAND kCmdPrevious and kCmdNext.
HWND CreateStatusBar(HWND parent, HINSTANCE instance);

// Resizes the bar and its fonts for a new DPI; the parent then moves it on WM_SIZE.
void LayoutStatusBar(UINT dpi);

int StatusBarHeight();

// Shows the image info on the left, leaving out empty fields. A negative percent means no image is loaded.
void SetStatus(std::wstring resolution, std::wstring size, long percent);

// Enables the previous/next buttons, which need the image to be a file.
void EnableStepButtons(bool enable);
