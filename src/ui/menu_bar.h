#pragma once

#include <windows.h>

enum Command : UINT { kCmdNew = 1, kCmdOpen, kCmdSaveAs };

// Creates the dark "File" bar across the top of parent's client area, sized for parent's DPI.
// Menu commands arrive at parent as WM_COMMAND.
HWND CreateMenuBar(HWND parent, HINSTANCE instance);

// Resizes the bar and its font for a new DPI; the parent then moves it on WM_SIZE.
void LayoutMenuBar(UINT dpi);

int MenuBarHeight();

// Opens the File menu; from the keyboard (Alt+F) its first item is selected.
void ShowFileMenu(bool keyboard);

// Enables the commands that need an open image.
void EnableImageCommands(bool enabled);
