# Image VIEWER

A minimal, fast native image viewer for Windows (Win32 + WIC + GDI).

## Build

Requires Visual Studio (or Build Tools) with the C++ workload. Run `build.bat`, or from a Developer Command Prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build\Image VIEWER.exe` (single file, no dependencies).

## Use

```
"Image VIEWER.exe" [photo.jpg]
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
"Image VIEWER.exe" --register
```

This opens Settings → Default apps, where you pick Image VIEWER. `--unregister` removes it.
