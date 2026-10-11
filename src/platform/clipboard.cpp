#include "platform/clipboard.h"

#include <shlobj_core.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr SIZE_T kMaxBytes = SIZE_T(512) << 20;

// The clipboard is shared with every other program, so its data is checked before anything parses it.
// Returns the size of a plain, uncompressed DIB (header, masks, colors, pixels) that fits in size bytes,
// with offset set to where its pixels start, or 0 if the data isn't one.
SIZE_T MeasureDib(const BYTE* data, SIZE_T size, UINT64& offset) {
    BITMAPINFOHEADER info;
    if (size < sizeof(info)) return 0;
    memcpy(&info, data, sizeof(info));  // clipboard data need not be aligned
    switch (info.biSize) {
        case sizeof(BITMAPINFOHEADER): case 52: case 56: case sizeof(BITMAPV4HEADER): case sizeof(BITMAPV5HEADER):
            break;
        default:
            return 0;
    }
    switch (info.biBitCount) {
        case 1: case 4: case 8: case 16: case 24: case 32:
            break;
        default:
            return 0;
    }
    // Only raw pixels: no embedded JPEG/PNG or RLE that would reach another parser.
    const bool bitfields = info.biCompression == BI_BITFIELDS;
    if (info.biCompression != BI_RGB && !(bitfields && (info.biBitCount == 16 || info.biBitCount == 32))) return 0;
    const UINT64 maxColors = info.biBitCount <= 8 ? 1u << info.biBitCount : 256;
    if (info.biWidth <= 0 || info.biHeight == 0 || info.biPlanes != 1 || info.biClrUsed > maxColors) return 0;

    const UINT64 masks = info.biSize == sizeof(BITMAPINFOHEADER) && bitfields ? 12 : 0;
    const UINT64 colors = info.biClrUsed ? info.biClrUsed : info.biBitCount <= 8 ? maxColors : 0;
    const UINT64 height = info.biHeight < 0 ? 0 - UINT64(INT64(info.biHeight)) : UINT64(info.biHeight);
    const UINT64 stride = (UINT64(info.biWidth) * info.biBitCount + 31) / 32 * 4;
    offset = info.biSize + masks + colors * 4;
    const UINT64 total = offset + stride * height;  // < 2^63: width, height < 2^31 and stride < 2^34
    return total <= size ? SIZE_T(total) : 0;
}

// Copies the open clipboard's image data into a stream: PNG if offered, else the DIB as a BMP file.
HRESULT CopyImageData(UINT format, Microsoft::WRL::ComPtr<IStream>& out) {
    const bool png = format != CF_DIB;
    const HGLOBAL data = GetClipboardData(format);
    const BYTE* p = data ? static_cast<const BYTE*>(GlobalLock(data)) : nullptr;
    if (!p) return S_FALSE;

    SIZE_T size = GlobalSize(data);
    HRESULT hr = size > kMaxBytes ? WINCODEC_ERR_IMAGESIZEOUTOFRANGE : S_OK;
    BITMAPFILEHEADER file{};
    if (SUCCEEDED(hr) && png) {
        if (size < 8 || memcmp(p, "\x89PNG\r\n\x1A\n", 8) != 0) hr = WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
    } else if (SUCCEEDED(hr)) {
        // Wrap the DIB in a BMP file header so the BMP decoder can read it.
        UINT64 offset = 0;
        size = MeasureDib(p, size, offset);
        if (size == 0) {
            hr = WINCODEC_ERR_BADIMAGE;
        } else {
            file.bfType = 0x4D42;  // "BM"
            file.bfSize = DWORD(sizeof(file) + size);
            file.bfOffBits = DWORD(sizeof(file) + offset);
        }
    }

    const SIZE_T header = png ? 0 : sizeof(file);
    HGLOBAL copy = SUCCEEDED(hr) ? GlobalAlloc(GMEM_MOVEABLE, header + size) : nullptr;
    if (SUCCEEDED(hr) && !copy) hr = E_OUTOFMEMORY;
    if (copy) {
        if (BYTE* d = static_cast<BYTE*>(GlobalLock(copy))) {
            std::copy_n(reinterpret_cast<const BYTE*>(&file), header, d);
            std::copy_n(p, size, d + header);
            GlobalUnlock(copy);
            hr = CreateStreamOnHGlobal(copy, TRUE, &out);
        } else {
            hr = E_OUTOFMEMORY;
        }
        if (FAILED(hr)) GlobalFree(copy);
    }
    GlobalUnlock(data);
    return hr;
}

// Sets path to the first file of a file list copied in Explorer (CF_HDROP).
HRESULT CopyFirstFile(std::wstring& path) {
    const HGLOBAL data = GetClipboardData(CF_HDROP);
    const BYTE* p = data ? static_cast<const BYTE*>(GlobalLock(data)) : nullptr;
    if (!p) return S_FALSE;
    const SIZE_T size = GlobalSize(data);
    HRESULT hr = S_FALSE;
    DROPFILES drop;
    if (size >= sizeof(drop)) {
        memcpy(&drop, p, sizeof(drop));
        if (drop.fWide && drop.pFiles >= sizeof(drop) && drop.pFiles < size) {
            // The name must end within the data and within the longest path Windows allows.
            const SIZE_T count = std::min<SIZE_T>((size - drop.pFiles) / sizeof(wchar_t), 32768);
            std::wstring name(count, L'\0');
            memcpy(name.data(), p + drop.pFiles, count * sizeof(wchar_t));  // need not be aligned
            name.resize(wcsnlen(name.c_str(), count));
            if (!name.empty() && name.size() < count) {
                path = std::move(name);
                hr = S_OK;
            }
        }
    }
    GlobalUnlock(data);
    return hr;
}

UINT PngFormat() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

}  // namespace

HRESULT ReadClipboardImage(HWND owner, Source& out) {
    if (!OpenClipboard(owner)) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = S_FALSE;
    if (IsClipboardFormatAvailable(PngFormat())) {
        hr = CopyImageData(PngFormat(), out.stream);
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        hr = CopyImageData(CF_DIB, out.stream);
    } else if (IsClipboardFormatAvailable(CF_HDROP)) {
        hr = CopyFirstFile(out.path);
    }
    CloseClipboard();
    return hr;
}

bool SetClipboardImage(HWND owner, HGLOBAL png, HGLOBAL dib) {
    bool done = false;
    if (OpenClipboard(owner)) {
        // PNG first: programs take the first format they understand, and it keeps every pixel and alpha.
        if (EmptyClipboard() && SetClipboardData(PngFormat(), png)) {
            png = nullptr;
            if (SetClipboardData(CF_DIB, dib)) {
                dib = nullptr;
                done = true;
            }
        }
        CloseClipboard();
    }
    if (png) GlobalFree(png);
    if (dib) GlobalFree(dib);
    return done;
}
