#include "platform/clipboard.h"

#include <shlobj_core.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

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
            // A color profile may be linked by file path, even on a network share: say sRGB instead, which
            // leaves every pixel as it is, since profiles are never applied.
            BITMAPV5HEADER v5;
            if (!png && size >= sizeof(v5) && (memcpy(&v5, p, sizeof(v5)), v5.bV5Size == sizeof(v5)) &&
                (v5.bV5CSType == PROFILE_LINKED || v5.bV5CSType == PROFILE_EMBEDDED)) {
                v5.bV5CSType = LCS_sRGB;
                v5.bV5ProfileData = v5.bV5ProfileSize = 0;
                memcpy(d + header, &v5, sizeof(v5));
            }
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

UINT HtmlFormat() {
    static const UINT format = RegisterClipboardFormatW(L"HTML Format");
    return format;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The file name at the end of an image URL, or empty if it isn't a plain name with an image extension.
std::wstring ImageNameOf(std::string_view url) {
    url = url.substr(0, url.find_first_of("?#"));
    url = url.substr(url.find_last_of('/') + 1);
    std::string bytes;
    for (size_t i = 0; i < url.size(); ++i) {
        if (url[i] != '%') {
            bytes += url[i];
            continue;
        }
        const int hi = i + 2 < url.size() ? HexDigit(url[i + 1]) : -1, lo = hi >= 0 ? HexDigit(url[i + 2]) : -1;
        if (lo < 0) return {};
        bytes += char(hi << 4 | lo);
        i += 2;
    }
    const int n = bytes.empty() || bytes.size() > 1024 ? 0
                  : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0);
    if (n <= 0 || n > 255) return {};
    std::wstring name(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), name.data(), n);
    // No path parts, control characters, or direction overrides that could make the name read differently.
    for (const wchar_t c : name) {
        if (c < 0x20 || c == 0x7F || wcschr(L"\\/:*?\"<>|", c) || c == 0x200E || c == 0x200F ||
            (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)) {
            return {};
        }
    }
    return name[0] != L'.' && IsImageName(name.c_str()) ? name : std::wstring();
}

// Sets name to the file name of the first <img src> in the clipboard's HTML, which browsers and Discord add
// next to a copied image. The URL is only read as text: nothing is ever fetched or opened.
void CopyHtmlImageName(std::wstring& name) {
    const HGLOBAL data = GetClipboardData(HtmlFormat());
    const char* p = data ? static_cast<const char*>(GlobalLock(data)) : nullptr;
    if (!p) return;
    const std::string_view html(p, std::min<SIZE_T>(GlobalSize(data), 64 << 10));
    const size_t img = html.find("<img");
    const std::string_view tag = img == html.npos ? "" : html.substr(img, html.find('>', img) - img);
    const size_t src = tag.find(" src=");
    if (src != tag.npos && src + 5 < tag.size() && (tag[src + 5] == '"' || tag[src + 5] == '\'')) {
        const size_t start = src + 6, end = tag.find(tag[src + 5], start);
        if (end != tag.npos) {
            std::wstring found = ImageNameOf(tag.substr(start, end - start));
            if (!found.empty()) name = std::move(found);
        }
    }
    GlobalUnlock(data);
}

}  // namespace

HRESULT ReadClipboardImage(HWND owner, Source& out, std::wstring& name, UINT64& pngBytes) {
    pngBytes = 0;
    if (!OpenClipboard(owner)) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = S_FALSE;
    if (IsClipboardFormatAvailable(PngFormat())) {
        hr = CopyImageData(PngFormat(), out.stream);
        if (hr == S_OK) pngBytes = GlobalSize(GetClipboardData(PngFormat()));
        name = L"Pasted image.png";
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        hr = CopyImageData(CF_DIB, out.stream);
        name = L"Pasted image.bmp";
    } else if (IsClipboardFormatAvailable(CF_HDROP)) {
        hr = CopyFirstFile(out.path);
    }
    if (hr == S_OK && out.stream) CopyHtmlImageName(name);
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
