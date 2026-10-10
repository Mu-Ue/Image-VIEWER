#include "platform/clipboard.h"

#include <wincodec.h>

#include <algorithm>

HRESULT CopyClipboardImage(HWND owner, Microsoft::WRL::ComPtr<IStream>& out) {
    static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (!OpenClipboard(owner)) return HRESULT_FROM_WIN32(GetLastError());
    const bool png = IsClipboardFormatAvailable(pngFormat);
    const HGLOBAL data = GetClipboardData(png ? pngFormat : CF_DIB);
    const BYTE* p = data ? static_cast<const BYTE*>(GlobalLock(data)) : nullptr;
    if (!p) {
        CloseClipboard();
        return S_FALSE;
    }

    const SIZE_T size = GlobalSize(data);
    HRESULT hr = S_OK;
    BITMAPFILEHEADER file{};
    if (!png) {
        // The clipboard is shared with other programs, so check the header fits before trusting it.
        const auto* info = reinterpret_cast<const BITMAPINFOHEADER*>(p);
        if (size < sizeof(BITMAPINFOHEADER) || info->biSize < sizeof(BITMAPINFOHEADER) || info->biSize > size) {
            hr = WINCODEC_ERR_BADIMAGE;
        } else {
            const UINT64 masks =
                info->biSize == sizeof(BITMAPINFOHEADER) && info->biCompression == BI_BITFIELDS ? 12 : 0;
            const UINT64 colors =
                info->biClrUsed ? info->biClrUsed : info->biBitCount <= 8 ? 1u << info->biBitCount : 0;
            const UINT64 offset = info->biSize + masks + colors * 4;
            if (offset > size || size > MAXDWORD - sizeof(file)) {
                hr = WINCODEC_ERR_BADIMAGE;
            } else {
                file.bfType = 0x4D42;  // "BM"
                file.bfSize = DWORD(sizeof(file) + size);
                file.bfOffBits = DWORD(sizeof(file) + offset);
            }
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
    CloseClipboard();
    return hr;
}
