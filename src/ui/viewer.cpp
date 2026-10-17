#include "ui/viewer.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string_view>
#include <thread>
#include <utility>

#include "config.h"
#include "imaging/animation.h"
#include "imaging/decoder.h"
#include "imaging/encoder.h"
#include "platform/clipboard.h"
#include "platform/file_dialog.h"
#include "ui/draw.h"
#include "ui/menu_bar.h"
#include "ui/scale.h"
#include "ui/status_bar.h"

namespace {

constexpr UINT_PTR kAnimationTimer = 1;
constexpr LONGLONG kFrameLeadUs = 1000;  // a pan is painted this long before the display refreshes
constexpr UINT kDecodedMessage = WM_APP;  // lParam is a finished Decode, which the window then owns
constexpr UINT kFramesMessage = WM_APP + 1;  // lParam is Frames, which the window then owns
constexpr DWORD kDecoderIdleMs = 10000;  // the decoder thread, and WIC with it, stays this long after an image

struct Viewer {
    HWND hwnd = nullptr;
    HWND bar = nullptr, status = nullptr;
    int barH = 0, statusH = 0;
    Source source;  // of the current image, re-read when saving
    std::wstring name;
    std::wstring resolution, size;  // shown in the status bar
    std::wstring stepTarget;  // the file a step is decoding, empty if none
    std::wstring stepFirst;  // the first file the step tried, to stop if every other file fails
    int stepDir = 1;

    Image image;
    Animation animation;  // of an animated GIF, drawn into image.bitmap
    bool awaitingFrames = false;  // the image is an animated GIF whose frames are still being read
    bool paused = false;  // the animation, while the window is minimized
    UINT64 shownId = 0;  // of the request whose image is shown
    HDC imageDC = nullptr;  // memory DC holding image.bitmap
    HBRUSH background = nullptr;

    float imageW = 0, imageH = 0;  // upright size in pixels
    float clientW = 0, clientH = 0;
    float zoom = 1, offsetX = 0, offsetY = 0;  // offset = image top-left in client pixels
    int shownX = 0, shownY = 0;  // the offset on screen, which a pan catches up to once per display refresh
    HANDLE frameTimer = nullptr;  // signaled when a pan should be painted
    bool frameArmed = false;
    bool fit = true;
    bool dragging = false;
    POINT dragFrom{};
    bool fullscreen = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};  // of the window before it went fullscreen
} g;

// Draws the image area where update (in image area coordinates) needs it.
void Render(HDC dc, HRGN update) {
    const float z = g.zoom;
    const RECT client{0, 0, LONG(g.clientW), LONG(g.clientH)};
    if (g.image.bitmap) {
        // Each screen pixel is the exact average of its image area, computed only where painted, one
        // rectangle at a time so the L-shaped strips a diagonal pan uncovers stay small. The drawn size
        // depends on the zoom alone, so a pan never changes which image area a pixel covers.
        const int x = g.shownX, y = g.shownY;
        const int w = int(std::lround(double(g.image.width) * z));
        const int h = int(std::lround(double(g.image.height) * z));
        union {
            RGNDATA data;
            BYTE bytes[sizeof(RGNDATAHEADER) + 8 * sizeof(RECT)];
        } region;
        RECT box;
        const RECT* rects = &box;
        DWORD count = 1;
        if (GetRegionData(update, sizeof(region), &region.data)) {
            rects = reinterpret_cast<const RECT*>(region.data.Buffer);
            count = region.data.rdh.nCount;
        } else {  // too many rectangles: draw their bounds
            GetClipBox(dc, &box);
        }
        bool drawn = true;
        for (DWORD i = 0; i < count && drawn; ++i) {
            RECT r;
            if (IntersectRect(&r, &rects[i], &client)) drawn = DrawScaled(dc, g.imageDC, x, y, w, h, r);
        }
        if (drawn) {
            if (x > 0 || y > 0 || x + w < client.right || y + h < client.bottom) {  // background shows
                ExcludeClipRect(dc, x, y, x + w, y + h);
                FillRect(dc, &client, g.background);
            }
            return;
        }
    }

    // Only stretch the part of the image that is on screen.
    const float ox = float(g.shownX), oy = float(g.shownY);
    const int sx0 = std::max(0, int(std::floor(-ox / z)));
    const int sy0 = std::max(0, int(std::floor(-oy / z)));
    const int sx1 = std::min(int(g.image.width), int(std::ceil((g.clientW - ox) / z)));
    const int sy1 = std::min(int(g.image.height), int(std::ceil((g.clientH - oy) / z)));
    if (sx1 > sx0 && sy1 > sy0) {
        const int dx0 = int(std::lround(ox + sx0 * z)), dy0 = int(std::lround(oy + sy0 * z));
        const int dx1 = int(std::lround(ox + sx1 * z)), dy1 = int(std::lround(oy + sy1 * z));
        SetStretchBltMode(dc, z >= 3.0f ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, dx0, dy0, dx1 - dx0, dy1 - dy0, g.imageDC, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
        ExcludeClipRect(dc, dx0, dy0, dx1, dy1);
    }
    FillRect(dc, &client, g.background);
}

float FitZoom() {
    if (g.imageW <= 0 || g.clientW <= 0 || g.clientH <= 0) return 1.0f;
    return std::min({1.0f, g.clientW / g.imageW, g.clientH / g.imageH});
}

// Centers the image on any axis where it fits, otherwise keeps the window covered.
void ClampOffset() {
    const float w = g.imageW * g.zoom, h = g.imageH * g.zoom;
    g.offsetX = w <= g.clientW ? (g.clientW - w) / 2 : std::clamp(g.offsetX, g.clientW - w, 0.0f);
    g.offsetY = h <= g.clientH ? (g.clientH - h) / 2 : std::clamp(g.offsetY, g.clientH - h, 0.0f);
}

// Whether dragging moves the image: it is larger than the window on some axis.
bool CanPan() {
    return g.image.bitmap && (g.imageW * g.zoom > g.clientW + 0.5f || g.imageH * g.zoom > g.clientH + 0.5f);
}

// Puts the offset on screen at the next full repaint.
void ShowOffset() {
    g.shownX = int(std::round(g.offsetX));
    g.shownY = int(std::round(g.offsetY));
}

// The zoom is shown relative to the source, which is larger than the bitmap if it was scaled down.
void UpdateStatus() {
    const double percent = g.image.bitmap ? double(g.zoom) * 100 * g.image.width / g.image.fullWidth : -1;
    SetStatus(g.resolution, g.size, std::lround(percent));
}

// Formats a byte count like Explorer does, in 1024-based units with one decimal above KB.
std::wstring FormatSize(ULONGLONG bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024 * 1024) return std::to_wstring((bytes + 512) / 1024) + L" KB";
    const wchar_t* unit = L" MB";
    ULONGLONG tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
    if (tenths >= 10240) {
        unit = L" GB";
        tenths = (tenths + 512) / 1024;
    }
    return std::to_wstring(tenths / 10) + L'.' + std::to_wstring(tenths % 10) + unit;
}

void ZoomAt(float zoom, float x, float y) {
    if (!g.image.bitmap) return;
    const float fitZoom = FitZoom();
    zoom = std::clamp(zoom, fitZoom, std::max(kMaxZoom, fitZoom));
    if ((g.zoom < 1.0f && zoom > 1.0f) || (g.zoom > 1.0f && zoom < 1.0f)) zoom = 1.0f;  // stop at actual size
    if (zoom == g.zoom) return;
    g.offsetX = x - (x - g.offsetX) * (zoom / g.zoom);
    g.offsetY = y - (y - g.offsetY) * (zoom / g.zoom);
    g.zoom = zoom;
    g.fit = zoom == fitZoom;
    ClampOffset();
    ShowOffset();
    UpdateStatus();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Moves the image with every mouse move, but paints it only once per display refresh, just before the
// compositor takes the frame: positions in between would never be seen.
void Pan(float dx, float dy) {
    g.offsetX += dx;
    g.offsetY += dy;
    ClampOffset();
    if (g.frameArmed || (int(std::round(g.offsetX)) == g.shownX && int(std::round(g.offsetY)) == g.shownY)) return;
    DWM_TIMING_INFO info{sizeof(info)};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) && info.qpcRefreshPeriod) {
        // The next paint deadline, a little before a refresh: a move after one waits for the next.
        const LONGLONG period = LONGLONG(info.qpcRefreshPeriod);
        LONGLONG deadline = LONGLONG(info.qpcVBlank) - freq.QuadPart * kFrameLeadUs / 1000000;
        if (now.QuadPart >= deadline) deadline += ((now.QuadPart - deadline) / period + 1) * period;
        // Relative, in 100 ns units.
        LARGE_INTEGER due{.QuadPart = -std::max((deadline - now.QuadPart) * 10000000 / freq.QuadPart, 1LL)};
        if (SetWaitableTimer(g.frameTimer, &due, 0, nullptr, nullptr, FALSE)) {
            g.frameArmed = true;
            return;
        }
    }
    PresentFrame();
}

// Shows the next frame of an animated GIF, repainting only the part of the canvas that changed.
void NextFrame() {
    Animation& a = g.animation;
    if (a.frames.empty()) return;
    if (a.current + 1 == a.frames.size() && a.plays && ++a.played == a.plays) {
        // Done: the last frame stays on the canvas, and the frames are no longer needed.
        KillTimer(g.hwnd, kAnimationTimer);
        a = {};
        return;
    }
    const Frame& previous = a.frames[a.current];
    a.current = (a.current + 1) % a.frames.size();
    GdiFlush();  // GDI may still be reading the canvas
    SetTimer(g.hwnd, kAnimationTimer, ShowFrame(a, a.current), nullptr);

    RECT changed = a.frames[a.current].rect;
    if (a.current == 0) {
        changed = {0, 0, LONG(a.width), LONG(a.height)};
    } else if (previous.disposal == 2 || previous.disposal == 3) {
        UnionRect(&changed, &changed, &previous.rect);
    }
    if (IsRectEmpty(&changed)) return;
    // The screen pixels whose image area overlaps the change, as Render scales, plus a pixel of margin.
    const UINT64 w = UINT64(std::lround(double(g.image.width) * g.zoom));
    const UINT64 h = UINT64(std::lround(double(g.image.height) * g.zoom));
    const LONG x = g.shownX, y = g.barH + g.shownY;
    const RECT r{x + LONG(changed.left * w / a.width) - 1, y + LONG(changed.top * h / a.height) - 1,
                 x + LONG((changed.right * w + a.width - 1) / a.width) + 1,
                 y + LONG((changed.bottom * h + a.height - 1) / a.height) + 1};
    InvalidateRect(g.hwnd, &r, FALSE);
}

void OnSize(float width, float height) {
    if (width <= 0 || height <= 0) return;  // minimized
    const float dx = (width - g.clientW) / 2, dy = (height - g.clientH) / 2;
    g.clientW = width;
    g.clientH = height;
    if (g.fit || g.zoom < FitZoom()) {
        g.zoom = FitZoom();
        g.fit = true;
    } else {
        g.offsetX += dx;  // keep the view centered on the same spot
        g.offsetY += dy;
    }
    ClampOffset();
    ShowOffset();
    UpdateStatus();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Sizes the window to the image (capped to the work area), or a default size when empty,
// and centers it on the active monitor.
void PlaceWindow() {
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const float workW = float(work.right - work.left), workH = float(work.bottom - work.top);

    const UINT dpi = GetDpiForWindow(g.hwnd);
    RECT rc{0, 0, MulDiv(960, dpi, 96), MulDiv(640, dpi, 96)};
    if (g.image.bitmap) {
        const float scale = std::min({1.0f, workW * 0.85f / g.imageW, workH * 0.85f / g.imageH});
        rc.right = std::max(LONG(std::lround(g.imageW * scale)), LONG(MulDiv(480, dpi, 96)));
        rc.bottom = std::max(LONG(std::lround(g.imageH * scale)), LONG(MulDiv(320, dpi, 96)));
    }
    rc.bottom += g.barH + g.statusH;
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(g.hwnd, nullptr, work.left + (int(workW) - w) / 2, work.top + (int(workH) - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Why a file can't be read or written, as a sentence, or empty if there is nothing more useful to say.
std::wstring FileErrorReason(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) return L"Access is denied.";
    if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) return L"Another program is using it.";
    if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == STG_E_MEDIUMFULL) return L"The disk is full.";
    return {};
}

// Why an image can't be opened, as a sentence, or empty if there is nothing more useful to say.
std::wstring ErrorReason(HRESULT hr) {
    if (hr == WINCODEC_ERR_UNKNOWNIMAGEFORMAT || hr == WINCODEC_ERR_BADIMAGE) return L"It isn't a supported image.";
    if (hr == WINCODEC_ERR_IMAGESIZEOUTOFRANGE) return L"It's too large to open.";
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND) {
        return L"This format isn't supported. Codecs for HEIC/AVIF are available from the Microsoft Store.";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        return L"The file doesn't exist.";
    }
    return FileErrorReason(hr);
}

void ShowError(HRESULT hr, const std::wstring& name) {
    std::wstring message = L"Can't open \"" + name + L"\".";
    if (const std::wstring reason = ErrorReason(hr); !reason.empty()) message += L"\n\n" + reason;
    MessageBoxW(g.hwnd, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

// Shows a decoded image, replacing the current one. bytes is the size of a pasted image's compressed data, if
// known; a file's size is read from the file.
void SetImage(const Image& image, std::wstring name, UINT64 bytes = 0) {
    SelectObject(g.imageDC, image.bitmap);
    if (g.image.bitmap) DeleteObject(g.image.bitmap);
    g.image = image;
    g.animation = {};
    g.awaitingFrames = false;
    g.paused = false;
    KillTimer(g.hwnd, kAnimationTimer);
    g.name = std::move(name);
    g.imageW = float(image.width);
    g.imageH = float(image.height);
    g.resolution = std::to_wstring(image.fullWidth) + L" × " + std::to_wstring(image.fullHeight);
    WIN32_FILE_ATTRIBUTE_DATA file;
    if (!g.source.path.empty() && GetFileAttributesExW(g.source.path.c_str(), GetFileExInfoStandard, &file)) {
        bytes = UINT64(file.nFileSizeHigh) << 32 | file.nFileSizeLow;
    }
    g.size = bytes ? FormatSize(bytes) : L"";
    g.zoom = FitZoom();
    g.fit = true;
    ClampOffset();
    ShowOffset();
    SetWindowTextW(g.hwnd, g.name.c_str());
    UpdateStatus();
    EnableImageCommands(true, !g.source.path.empty());
    EnableStepButtons(!g.source.path.empty());
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Runs work on a new thread with its own COM apartment, setting hr to its result. The UI thread never
// starts COM, so when the thread ends COM unloads WIC and its codecs instead of keeping them in memory.
template <class Work>
std::thread ComThread(HRESULT& hr, Work work) {
    return std::thread([&hr, work] {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            hr = work();
            CoUninitialize();
        }
    });
}

// How a decode was asked for, which decides what a failure does. Animate only reads the frames of the
// animated GIF shown.
enum class Ask { Open, Paste, Step, Animate };

// An image for the decoder thread, and what came of it.
struct Decode {
    UINT64 id;
    Ask ask;
    Source src;
    std::wstring name;
    UINT64 bytes;  // the compressed size of a pasted image, if known
    HRESULT hr = E_FAIL;
    Image image;
    bool animated = false;  // image is the first frame of an animated GIF, whose Frames follow
};

// The frames of the animated GIF a Decode showed.
struct Frames {
    UINT64 id;  // of the Decode
    Animation animation;
};

// The decoder thread decodes only the latest request: a new one replaces any still waiting, and a result
// that is no longer the latest is dropped, so the window never waits and held keys never queue up images.
// It keeps WIC and its codecs loaded while images keep coming and ends once idle, which unloads them; the
// UI thread never starts COM. Nothing here has a destructor, so exiting while it decodes is safe.
struct {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    Decode* next = nullptr;  // waiting to start
    bool running = false;
    std::atomic<bool> waiting{false};  // next is set, which stops reading an animation's frames
    std::atomic<UINT64> latest{0};  // the id of the latest request
} worker;

void Discard(Decode* d) {
    if (d->image.bitmap) DeleteObject(d->image.bitmap);
    delete d;
}

// The next request for the decoder thread, or null once it has been idle long enough to end.
Decode* NextDecode() {
    AcquireSRWLockExclusive(&worker.lock);
    while (!worker.next) {
        if (!SleepConditionVariableSRW(&worker.wake, &worker.lock, kDecoderIdleMs, 0) && !worker.next) {
            worker.running = false;  // the next request starts a new thread
            break;
        }
    }
    Decode* const d = std::exchange(worker.next, nullptr);
    worker.waiting = false;
    ReleaseSRWLockExclusive(&worker.lock);
    return d;
}

void RunDecoder() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        const WicFactory keep;  // holds windowscodecs.dll, so each image doesn't load it again
        while (Decode* const d = NextDecode()) {
            const UINT64 id = d->id;
            const Source src = d->src;  // for the frames, as d is handed over
            bool animate = d->ask == Ask::Animate;
            if (animate) {
                delete d;
            } else {
                if (id == worker.latest) d->hr = FAILED(com) ? com : DecodeImage(d->src, d->image, &d->animated);
                animate = SUCCEEDED(d->hr) && d->animated;
                if (id != worker.latest || !PostMessageW(g.hwnd, kDecodedMessage, 0, LPARAM(d))) {
                    Discard(d);
                    animate = false;
                }
            }
            // An animated GIF shows its first frame at once and plays once every frame is read, which a new
            // request interrupts.
            if (animate && SUCCEEDED(com) && id == worker.latest) {
                auto* const f = new Frames{id};
                if (FAILED(DecodeAnimation(src, f->animation, worker.waiting)) ||
                    !PostMessageW(g.hwnd, kFramesMessage, 0, LPARAM(f))) {
                    delete f;
                }
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

// Decodes an image on the decoder thread, to be shown by OnDecoded.
void Request(Ask ask, Source src, std::wstring name, UINT64 bytes = 0) {
    if (ask != Ask::Step) g.stepTarget.clear();
    auto* const d = new Decode{worker.latest + 1, ask, std::move(src), std::move(name), bytes};
    worker.latest = d->id;
    AcquireSRWLockExclusive(&worker.lock);
    Decode* const replaced = std::exchange(worker.next, d);
    worker.waiting = true;
    const bool start = !worker.running;
    worker.running = true;
    ReleaseSRWLockExclusive(&worker.lock);
    WakeConditionVariable(&worker.wake);
    if (replaced) Discard(replaced);
    if (!start) return;
    const HANDLE thread = CreateThread(nullptr, 0, [](void*) -> DWORD { RunDecoder(); return 0; }, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        return;
    }
    // No thread: fail the request as if it had been decoded.
    AcquireSRWLockExclusive(&worker.lock);
    worker.running = false;
    worker.waiting = false;
    Decode* const failed = std::exchange(worker.next, nullptr);
    ReleaseSRWLockExclusive(&worker.lock);
    if (failed) {
        failed->hr = HRESULT_FROM_WIN32(GetLastError());
        if (!PostMessageW(g.hwnd, kDecodedMessage, 0, LPARAM(failed))) Discard(failed);
    }
}

// Reads the frames of the animated GIF shown.
void RequestFrames() {
    Request(Ask::Animate, g.source, {});
    g.shownId = worker.latest;
}

// Drops the request in progress, if any.
void CancelDecode() {
    worker.latest = worker.latest + 1;
    AcquireSRWLockExclusive(&worker.lock);
    Decode* const waiting = std::exchange(worker.next, nullptr);
    worker.waiting = false;
    ReleaseSRWLockExclusive(&worker.lock);
    if (waiting) Discard(waiting);
    g.stepTarget.clear();
    if (g.awaitingFrames) RequestFrames();  // the request may have stopped them being read
}

std::wstring FileName(const std::wstring& path) {
    return path.substr(path.find_last_of(L"\\/") + 1);
}

void Open() {
    std::wstring path = RunFileDialog(g.hwnd, L"open", {});
    if (path.empty()) return;
    std::wstring name = FileName(path);
    Request(Ask::Open, {std::move(path)}, std::move(name));
}

// File names are not case-sensitive.
bool SamePath(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL;
}

// Orders names like Explorer ("2" before "10"), breaking ties so no two names compare equal.
int CompareNames(const wchar_t* a, const wchar_t* b) {
    const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a, -1, b, -1,
                                  nullptr, nullptr, 0);
    return r != CSTR_EQUAL ? r - CSTR_EQUAL : CompareStringOrdinal(a, -1, b, -1, FALSE) - CSTR_EQUAL;
}

// Sets around[0] and around[1] to the images before and after path in its folder, wrapping around at the ends.
// Both are empty if the folder holds no other image. Lists the folder each time, so it is never out of date.
void Neighbors(const std::wstring& path, std::wstring (&around)[2]) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
    const wchar_t* name = path.c_str() + dir.size();
    // Closest before and after, and the last and first for wrapping around.
    std::wstring before, after, last, first;
    WIN32_FIND_DATAW fd;
    const HANDLE find = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                         nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* f = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsImageName(f) || SamePath(f, name)) continue;
            const int c = CompareNames(f, name);
            if (c < 0) {
                if (before.empty() || CompareNames(f, before.c_str()) > 0) before = f;
                if (first.empty() || CompareNames(f, first.c_str()) < 0) first = f;
            } else if (c > 0) {
                if (after.empty() || CompareNames(f, after.c_str()) < 0) after = f;
                if (last.empty() || CompareNames(f, last.c_str()) > 0) last = f;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (before.empty()) before = last;
    if (after.empty()) after = first;
    around[0] = before.empty() ? L"" : dir + before;
    around[1] = after.empty() ? L"" : dir + after;
}

// Asks for the image after (or before) from in the step's direction.
void StepFrom(std::wstring from) {
    std::wstring around[2];
    Neighbors(from, around);
    std::wstring& to = around[g.stepDir > 0];
    // Back at the image shown, nothing else, or every other file failed. Cancel only a step still being decoded:
    // cancelling would also restart reading the frames of the animated GIF shown.
    if (to.empty() || SamePath(to, g.source.path) || SamePath(to, g.stepFirst)) {
        if (!g.stepTarget.empty()) CancelDecode();
        return;
    }
    if (g.stepFirst.empty()) g.stepFirst = to;
    g.stepTarget = to;
    std::wstring name = FileName(to);
    Request(Ask::Step, {std::move(to)}, std::move(name));
}

// Shows the previous (step < 0) or next image in the folder, silently skipping files that can't be opened.
// Steps go on from the image last asked for, so quick presses add up while the decoder catches up.
void Step(int step) {
    if (g.source.path.empty()) return;
    g.stepDir = step;
    g.stepFirst.clear();
    StepFrom(g.stepTarget.empty() ? g.source.path : g.stepTarget);
}

void SaveAs() {
    if (!g.image.bitmap) return;
    const std::wstring path = RunFileDialog(g.hwnd, L"save", g.name);
    if (path.empty()) return;
    HRESULT hr;
    ComThread(hr, [&] { return SaveImage(g.source, path); }).join();
    if (FAILED(hr)) {
        std::wstring message = L"Can't save \"" + path + L"\".";
        if (const std::wstring reason = FileErrorReason(hr); !reason.empty()) message += L"\n\n" + reason;
        MessageBoxW(g.hwnd, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
    }
}

// Switches between the normal window and a borderless one covering its monitor, without the bars.
void ToggleFullscreen() {
    g.fullscreen = !g.fullscreen;
    const LONG_PTR style = GetWindowLongPtrW(g.hwnd, GWL_STYLE);
    // The bar height changes first, so the resize lays out the image area for it.
    if (g.fullscreen) {
        GetWindowPlacement(g.hwnd, &g.placement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        g.barH = g.statusH = 0;
        ShowWindow(g.bar, SW_HIDE);
        ShowWindow(g.status, SW_HIDE);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(g.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        g.barH = MenuBarHeight();
        g.statusH = StatusBarHeight();
        ShowWindow(g.bar, SW_SHOW);
        ShowWindow(g.status, SW_SHOW);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g.hwnd, &g.placement);
        SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

// Puts the image on the clipboard at full quality, not the path of its file.
void Copy() {
    if (!g.image.bitmap) return;
    HGLOBAL png = nullptr, dib = nullptr;
    HRESULT hr;
    ComThread(hr, [&] { return EncodeClipboardImage(g.source, png, dib); }).join();
    if (FAILED(hr) || !SetClipboardImage(g.hwnd, png, dib)) {
        MessageBoxW(g.hwnd, L"Can't copy the image.", kAppName, MB_OK | MB_ICONERROR);
    }
}

void ShowPasteError(HRESULT hr) {
    const std::wstring reason = ErrorReason(hr);
    ShowStatusMessage(L"Can't paste. " + (reason.empty() ? L"It isn't a supported image." : reason));
}

// Shows the clipboard image, or the first file copied in Explorer. Problems go to the status bar.
void Paste() {
    Source src;
    std::wstring name;
    UINT64 bytes;
    const HRESULT hr = ReadClipboardImage(g.hwnd, src, name, bytes);
    if (hr == S_OK) {
        if (!src.path.empty()) name = FileName(src.path);
        Request(Ask::Paste, std::move(src), std::move(name), bytes);
    } else if (hr == S_FALSE) {
        ShowStatusMessage(L"Nothing to paste: the clipboard doesn't hold an image.");
    } else {
        ShowPasteError(hr);
    }
}

// Shows a decoded image, or what went wrong, unless a newer request replaced it.
void OnDecoded(Decode* d) {
    if (d->id != worker.latest) return Discard(d);
    if (FAILED(d->hr)) {
        const Ask ask = d->ask;
        const HRESULT hr = d->hr;
        std::wstring name = std::move(d->name), path = std::move(d->src.path);
        Discard(d);
        if (ask == Ask::Open) {
            ShowError(hr, name);
        } else if (ask == Ask::Paste) {
            ShowPasteError(hr);
        } else {
            StepFrom(std::move(path));  // skip it
        }
        return;
    }
    g.stepTarget.clear();
    // A pasted copy is no longer needed once the bitmap holds every pixel of it.
    if (d->src.stream && d->image.exact) d->src = {.bitmap = d->image.bitmap};
    g.source = std::move(d->src);
    SetImage(d->image, std::move(d->name), d->bytes);
    g.shownId = d->id;
    g.awaitingFrames = d->animated;
    delete d;
}

// Plays the animated GIF shown once all its frames are read.
void OnFrames(Frames* f) {
    Animation& a = f->animation;
    DIBSECTION ds{};
    if (f->id == g.shownId && g.awaitingFrames &&
        GetObjectW(g.image.bitmap, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBitsPixel == 24 &&
        UINT(ds.dsBm.bmWidth) == a.width && UINT(ds.dsBm.bmHeight) == a.height) {
        a.bits = static_cast<BYTE*>(ds.dsBm.bmBits);
        a.stride = UINT(ds.dsBm.bmWidthBytes);
        g.animation = std::move(a);
        g.awaitingFrames = false;
        GdiFlush();
        ShowFrame(g.animation, 0);  // the same pixels, but keeping what the frame's disposal needs
        if (IsIconic(g.hwnd)) {
            g.paused = true;
        } else {
            SetTimer(g.hwnd, kAnimationTimer, g.animation.frames[0].delay, nullptr);
        }
    }
    delete f;
}

void RunCommand(UINT cmd) {
    const float cx = g.clientW / 2, cy = g.clientH / 2;
    switch (cmd) {
        case kCmdOpen: Open(); break;
        case kCmdCopy: Copy(); break;
        case kCmdPaste: Paste(); break;
        case kCmdSaveAs: SaveAs(); break;
        case kCmdZoomIn: ZoomAt(g.zoom * kZoomStep, cx, cy); break;
        case kCmdZoomOut: ZoomAt(g.zoom / kZoomStep, cx, cy); break;
        case kCmdFit: ZoomAt(FitZoom(), cx, cy); break;
        case kCmdActualSize: ZoomAt(1.0f, cx, cy); break;
        case kCmdPrevious: Step(-1); break;
        case kCmdNext: Step(1); break;
        case kCmdFullscreen: ToggleFullscreen(); break;
    }
}

// A dark title bar that blends into the menu bar (its color is Windows 11 only), or the system's own in
// High Contrast.
void ThemeFrame(HWND hwnd) {
    const BOOL dark = !theme.highContrast;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = theme.highContrast ? DWMWA_COLOR_DEFAULT : kChrome;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
}

// Follows High Contrast and system color changes in the frame, bars and menus.
void OnThemeChanged(HWND hwnd) {
    LoadTheme();
    ThemeFrame(hwnd);
    LayoutMenuBar(GetDpiForWindow(hwnd));  // rebuilds the menus in the new colors
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const HINSTANCE instance = reinterpret_cast<const CREATESTRUCTW*>(lp)->hInstance;
            g.bar = CreateMenuBar(hwnd, instance);
            g.barH = MenuBarHeight();
            g.status = g.bar ? CreateStatusBar(hwnd, instance) : nullptr;
            g.statusH = StatusBarHeight();
            return g.status ? 0 : -1;
        }
        case WM_PAINT: {
            const HRGN update = CreateRectRgn(0, 0, 0, 0);
            GetUpdateRgn(hwnd, update, FALSE);
            OffsetRgn(update, 0, -g.barH);
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(hwnd, &ps);
            SetViewportOrgEx(dc, 0, g.barH, nullptr);  // the image area starts below the bar
            Render(dc, update);
            EndPaint(hwnd, &ps);
            DeleteObject(update);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {  // the menu bar's empty space moves the window, like the title bar
            const LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            return hit == HTCLIENT && pt.y < g.barH ? HTCAPTION : hit;
        }
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED && !g.animation.frames.empty()) {  // nothing shows it
                KillTimer(hwnd, kAnimationTimer);
                g.paused = true;
            } else if (wp != SIZE_MINIMIZED && g.paused) {
                g.paused = false;
                SetTimer(hwnd, kAnimationTimer, g.animation.frames[g.animation.current].delay, nullptr);
            }
            MoveWindow(g.bar, 0, 0, LOWORD(lp), g.barH, TRUE);
            MoveWindow(g.status, 0, HIWORD(lp) - g.statusH, LOWORD(lp), g.statusH, TRUE);
            OnSize(float(LOWORD(lp)), float(HIWORD(lp) - g.barH - g.statusH));
            return 0;
        case WM_MOUSEWHEEL: {  // also delivered for touchpad pinch
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            pt.y -= g.barH;
            const float notches = GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA);
            ZoomAt(g.zoom * std::pow(kZoomStep, notches), float(pt.x), float(pt.y));
            return 0;
        }
        case WM_LBUTTONDOWN:
            g.dragging = true;
            g.dragFrom = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetCapture(hwnd);
            if (CanPan()) SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return 0;
        case WM_SETCURSOR:  // the move cursor while dragging an image that doesn't fit
            if (g.dragging && CanPan()) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            if (g.dragging) {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                Pan(float(pt.x - g.dragFrom.x), float(pt.y - g.dragFrom.y));
                g.dragFrom = pt;
            }
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            g.dragging = false;
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return 0;
        case WM_LBUTTONDBLCLK: {  // toggle fit <-> 100% (2x for images smaller than the window)
            const float fitZoom = FitZoom();
            ZoomAt(g.fit ? (fitZoom < 1.0f ? 1.0f : 2.0f) : fitZoom,
                   float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp) - g.barH));
            return 0;
        }
        case WM_COMMAND:
            RunCommand(LOWORD(wp));
            return 0;
        case WM_DROPFILES: {  // opens the first file dropped from Explorer
            const HDROP drop = HDROP(wp);
            std::wstring path = DroppedFile(drop);
            DragFinish(drop);
            if (!path.empty()) {
                std::wstring name = FileName(path);
                Request(Ask::Open, {std::move(path)}, std::move(name));
            }
            return 0;
        }
        case kDecodedMessage:
            OnDecoded(reinterpret_cast<Decode*>(lp));
            return 0;
        case kFramesMessage:
            OnFrames(reinterpret_cast<Frames*>(lp));
            return 0;
        case WM_TIMER:
            if (wp == kAnimationTimer) NextFrame();
            return 0;
        case WM_SYSCOMMAND:  // no system menu: Alt, Alt+Space, F10 and clicking the title bar icon
            if ((wp & 0xFFF0) == SC_MOUSEMENU || (wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_CONTEXTMENU:  // no system menu on right-clicking the title bar
            return 0;
        case WM_SYSCHAR:
            if (wp == 'f' || wp == 'F' || wp == 'v' || wp == 'V') {
                ShowMenu(wp == 'f' || wp == 'F' ? kMenuFile : kMenuView, true);
                return 0;
            }
            break;
        case WM_KEYDOWN: {
            if (GetKeyState(VK_CONTROL) < 0) {
                switch (wp) {
                    case 'O': RunCommand(kCmdOpen); break;
                    case 'S': RunCommand(kCmdSaveAs); break;
                    case 'C': RunCommand(kCmdCopy); break;
                    case 'V': RunCommand(kCmdPaste); break;
                }
                return 0;
            }
            switch (wp) {
                case VK_OEM_PLUS: case VK_ADD: RunCommand(kCmdZoomIn); break;
                case VK_OEM_MINUS: case VK_SUBTRACT: RunCommand(kCmdZoomOut); break;
                case '0': case VK_NUMPAD0: RunCommand(kCmdFit); break;
                case '1': case VK_NUMPAD1: RunCommand(kCmdActualSize); break;
                case VK_LEFT: RunCommand(kCmdPrevious); break;
                case VK_RIGHT: RunCommand(kCmdNext); break;
                case VK_F11: RunCommand(kCmdFullscreen); break;
                case VK_ESCAPE:
                    if (g.fullscreen) {
                        ToggleFullscreen();
                    } else {
                        DestroyWindow(hwnd);
                    }
                    break;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            LayoutMenuBar(HIWORD(wp));
            LayoutStatusBar(HIWORD(wp));  // after the menu bar, whose height it shares
            g.barH = g.fullscreen ? 0 : MenuBarHeight();
            g.statusH = g.fullscreen ? 0 : StatusBarHeight();
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST) OnThemeChanged(hwnd);
            break;
        case WM_SYSCOLORCHANGE:
            OnThemeChanged(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateMainWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g.background = CreateSolidBrush(kBackground);
    wc.lpszClassName = kAppId;
    if (!RegisterClassExW(&wc)) return nullptr;

    // WS_EX_ACCEPTFILES is what DragAcceptFiles sets, without loading shell32 at startup.
    LoadTheme();
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, kAppId, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, nullptr);
    if (hwnd) ThemeFrame(hwnd);
    return hwnd;
}

}  // namespace

// Scrolls the drawn pixels to the current offset, computing only the uncovered strips: drawn pixels are
// exact relative to the image's top left.
void PresentFrame() {
    g.frameArmed = false;
    const int x = int(std::round(g.offsetX)), y = int(std::round(g.offsetY));
    const int sx = x - g.shownX, sy = y - g.shownY;
    if (sx == 0 && sy == 0) return;
    g.shownX = x;
    g.shownY = y;
    const RECT area{0, g.barH, LONG(g.clientW), g.barH + LONG(g.clientH)};
    ScrollWindowEx(g.hwnd, sx, sy, &area, &area, nullptr, nullptr, SW_INVALIDATE);
    UpdateWindow(g.hwnd);
}

HANDLE FrameTimer() {
    return g.frameTimer;
}

bool StartViewer(HINSTANCE instance, std::wstring path, int showCmd) {
    g.source.path = std::move(path);
    g.name = g.source.path.substr(g.source.path.find_last_of(L"\\/") + 1);

    // Decode on a worker thread while the window is set up.
    Image image;
    bool animated = false;
    HRESULT decodeHr = S_OK;
    std::thread decoder;
    if (!g.source.path.empty()) {
        decoder = ComThread(decodeHr, [&] { return DecodeImage(g.source, image, &animated); });
    }

    g.hwnd = CreateMainWindow(instance);
    if (decoder.joinable()) decoder.join();
    if (FAILED(decodeHr)) {
        ShowError(decodeHr, g.name);
        return false;
    }
    if (!g.hwnd) return false;

    g.imageDC = CreateCompatibleDC(nullptr);
    g.frameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // High resolution timers need Windows 10 1803; older versions pace pans less precisely.
    if (!g.frameTimer) g.frameTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (!g.imageDC || !g.frameTimer) return false;
    if (image.bitmap) SetImage(image, g.name);
    if (animated) {
        g.awaitingFrames = true;
        RequestFrames();
    }

    PlaceWindow();
    ShowWindow(g.hwnd, showCmd);
    UpdateWindow(g.hwnd);
    return true;
}
