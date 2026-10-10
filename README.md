# Image Viewer

A minimal, fast native image viewer for Windows (Win32 + WIC + Direct2D).

## Build

Requires Visual Studio (or Build Tools) with the C++ workload. From a Developer Command Prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build\ImageViewer.exe` (single file, no dependencies).

## Use

```
ImageViewer.exe [photo.jpg]
```

| Input                    | Action                    |
| ------------------------ | ------------------------- |
| Mouse wheel / pinch      | Zoom at cursor            |
| Drag                     | Pan                       |
| Double-click             | Toggle fit / 100%         |
| `+` / `-`                | Zoom in / out             |
| `0` / `1`                | Fit / 100%                |
| `Ctrl+N`                 | New (close image)         |
| `Ctrl+O`                 | Open                      |
| `Ctrl+V`                 | Paste image               |
| `Ctrl+S`                 | Save as PNG or JPEG       |
| `Esc`                    | Close                     |

## Set as default viewer

Put the exe where it will stay, then run:

```
ImageViewer.exe --register
```

This opens Settings → Default apps, where you pick Image Viewer. `--unregister` removes it.
