#pragma once

#include <windows.h>

enum Command : UINT {
    kCmdNew = 1, kCmdOpen, kCmdCopy, kCmdPaste, kCmdSaveAs,
    kCmdZoomIn, kCmdZoomOut, kCmdFit, kCmdActualSize, kCmdPrevious, kCmdNext, kCmdFullscreen,
};

enum MenuIndex { kMenuFile, kMenuView };

// Creates the dark "File  View" bar across the top of parent's client area, sized for parent's DPI.
// Menu commands arrive at parent as WM_COMMAND.
HWND CreateMenuBar(HWND parent, HINSTANCE instance);

// Resizes the bar, its font and its menus for a new DPI; the parent then moves it on WM_SIZE.
void LayoutMenuBar(UINT dpi);

int MenuBarHeight();

// Opens a menu; from the keyboard (Alt+F, Alt+V) its first item is selected.
void ShowMenu(MenuIndex index, bool keyboard);

// Enables the commands that need an open image, and those that need it to be a file (previous/next).
void EnableImageCommands(bool image, bool file);
