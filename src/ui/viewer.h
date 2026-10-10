#pragma once

#include <windows.h>

#include <string>

// Creates and shows the main window, with the image at path if it is not empty.
// Returns false if the window can't be created or the image can't be opened.
bool StartViewer(HINSTANCE instance, std::wstring path, int showCmd);
