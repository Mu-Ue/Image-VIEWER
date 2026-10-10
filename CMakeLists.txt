cmake_minimum_required(VERSION 3.21)
project(ImageViewer LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributables
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # lets VS Code IntelliSense see the real flags

add_executable(ImageViewer WIN32
    src/main.cpp
    src/imaging/decoder.cpp
    src/imaging/encoder.cpp
    src/platform/clipboard.cpp
    src/platform/file_dialog.cpp
    src/platform/registration.cpp
    src/ui/menu_bar.cpp
    src/ui/viewer.cpp
    res/app.rc
    res/app.manifest)
target_include_directories(ImageViewer PRIVATE src)
target_compile_definitions(ImageViewer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ImageViewer PRIVATE /W4 /permissive- /utf-8 /sdl /guard:cf)
target_link_libraries(ImageViewer PRIVATE gdi32 windowscodecs dwmapi delayimp)
# Only needed for the file picker and --register; keep them off the startup path.
target_link_options(ImageViewer PRIVATE /DELAYLOAD:comdlg32.dll /DELAYLOAD:shell32.dll)
# Control-flow guard, shadow stack, and DLL imports from System32 only (no planted DLLs next to the exe).
target_link_options(ImageViewer PRIVATE /GUARD:CF /CETCOMPAT /DEPENDENTLOADFLAG:0x800)
