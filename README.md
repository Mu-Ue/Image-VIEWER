<p align="center"><img src="docs/icon.png" width="128" alt="ImageVIEWER icon"></p>

# ImageVIEWER

A lightweight and highly optimized image viewer for Windows, built on native APIs.

![ImageVIEWER showing its own icon](docs/screenshot.png)

## Build

Requires Visual Studio or the Build Tools with the C++ workload. Run `build.bat`, or use a Developer Command Prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build\ImageVIEWER.exe`, a single file with no dependencies.

## Use

```
ImageVIEWER.exe [photo.jpg]
```

| Input                    | Action                    |
| ------------------------ | ------------------------- |
| Mouse wheel / pinch      | Zoom at cursor            |
| Drag                     | Pan                       |
| Double-click             | Toggle fit / 100%         |
| `+` / `-`                | Zoom in / out             |
| `0` / `1`                | Fit / 100%                |
| `←` / `→`                | Previous / next image     |
| `F11`                    | Fullscreen                |
| `Ctrl+O`                 | Open                      |
| `Ctrl+C`                 | Copy image                |
| `Ctrl+V`                 | Paste image or image file |
| `Ctrl+S`                 | Save as                   |
| `Esc`                    | Exit fullscreen / close   |
| `Alt+F` / `Alt+V`        | File / View menu          |

## Set as default viewer

Put the exe and `register.bat` where they will stay, then double-click `register.bat`. Or run:

```
ImageVIEWER.exe --register
```

This adds ImageVIEWER to the Start menu and opens Settings → Default apps, where you pick it. Run `--unregister` to remove both.
