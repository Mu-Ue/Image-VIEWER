#include "imaging/animation.h"

#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "config.h"

using Microsoft::WRL::ComPtr;

namespace {

// Reads an unsigned metadata value, or returns fallback if it is missing.
UINT ReadUint(IWICMetadataQueryReader* reader, const wchar_t* query, UINT fallback) {
    PROPVARIANT value;
    PropVariantInit(&value);
    if (FAILED(reader->GetMetadataByName(query, &value))) return fallback;
    UINT result = fallback;
    switch (value.vt) {
        case VT_UI1: result = value.bVal; break;
        case VT_UI2: result = value.uiVal; break;
        case VT_UI4: result = value.ulVal; break;
        case VT_BOOL: result = value.boolVal != VARIANT_FALSE; break;
    }
    PropVariantClear(&value);
    return result;
}

LONG Width(const RECT& r) { return r.right - r.left; }
LONG Height(const RECT& r) { return r.bottom - r.top; }

}  // namespace

HRESULT ReadAnimation(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, Animation& out) {
    UINT count = 0;
    ComPtr<IWICMetadataQueryReader> reader;
    HRESULT hr = decoder->GetFrameCount(&count);
    if (SUCCEEDED(hr) && count < 2) hr = WINCODEC_ERR_UNSUPPORTEDOPERATION;
    if (SUCCEEDED(hr)) hr = decoder->GetMetadataQueryReader(&reader);
    if (FAILED(hr)) return hr;
    const UINT width = ReadUint(reader.Get(), L"/logscrdesc/Width", 0);
    const UINT height = ReadUint(reader.Get(), L"/logscrdesc/Height", 0);
    if (width == 0 || height == 0 || width > kMaxBitmapSize || height > kMaxBitmapSize) {
        return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    }

    ComPtr<IWICPalette> palette;
    hr = wic->CreatePalette(&palette);
    if (FAILED(hr)) return hr;
    std::vector<ComPtr<IWICBitmapFrameDecode>> decoded(count);
    std::vector<Frame> frames(count);
    UINT64 total = 0, budget = 0;  // index bytes, and those plus the frames' own size
    size_t saved = 0;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode>& frame = decoded[i];
        WICPixelFormatGUID format;
        UINT w = 0, h = 0;
        ComPtr<IWICMetadataQueryReader> meta;
        hr = decoder->GetFrame(i, &frame);
        if (SUCCEEDED(hr)) hr = frame->GetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != GUID_WICPixelFormat8bppIndexed) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
        if (SUCCEEDED(hr)) hr = frame->GetMetadataQueryReader(&meta);
        if (SUCCEEDED(hr)) hr = frame->CopyPalette(palette.Get());
        Frame& f = frames[i];
        UINT colors = 0;
        if (SUCCEEDED(hr)) hr = palette->GetColors(256, f.colors, &colors);
        if (FAILED(hr)) return hr;
        std::fill(f.colors + colors, f.colors + 256, 0u);
        for (UINT32& c : f.colors) c &= 0xFFFFFF;

        // Frame positions and sizes are 16-bit, so these sums cannot overflow.
        const UINT left = ReadUint(meta.Get(), L"/imgdesc/Left", 0), top = ReadUint(meta.Get(), L"/imgdesc/Top", 0);
        f.rect = {LONG(std::min(left, width)), LONG(std::min(top, height)), LONG(std::min(left + w, width)),
                  LONG(std::min(top + h, height))};
        const UINT delay = ReadUint(meta.Get(), L"/grctlext/Delay", 0);  // in 1/100 s
        f.delay = delay <= 1 ? 100 : delay * 10;  // like browsers, which treat 0 and 1 as unset
        f.disposal = BYTE(ReadUint(meta.Get(), L"/grctlext/Disposal", 0));
        f.transparent = ReadUint(meta.Get(), L"/grctlext/TransparencyFlag", 0)
                            ? int(ReadUint(meta.Get(), L"/grctlext/TransparentColorIndex", 0))
                            : -1;
        const UINT64 area = UINT64(Width(f.rect)) * Height(f.rect);
        f.offset = size_t(total);
        total += area;
        budget += area + sizeof(Frame);
        if (budget > kMaxAnimationBytes) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
        if (f.disposal == 3) saved = std::max(saved, size_t(area) * 3);
    }

    Pages<BYTE> pixels = AllocPages<BYTE>(std::max<size_t>(1, size_t(total)));
    if (!pixels) return E_OUTOFMEMORY;
    for (UINT i = 0; i < count; ++i) {
        const Frame& f = frames[i];
        const UINT w = UINT(Width(f.rect)), h = UINT(Height(f.rect));
        if (w == 0 || h == 0) continue;
        const WICRect rect{0, 0, INT(w), INT(h)};  // clipping only ever trims the right and bottom
        hr = decoded[i]->CopyPixels(&rect, w, w * h, pixels.get() + f.offset);
        if (FAILED(hr)) return hr;
    }

    out.frames = std::move(frames);
    out.pixels = std::move(pixels);
    out.saved.resize(saved);
    out.current = 0;
    out.width = width;
    out.height = height;
    return S_OK;
}

UINT ShowFrame(Animation& a, size_t i) {
    const BYTE bg[3] = {GetBValue(kBackground), GetGValue(kBackground), GetRValue(kBackground)};
    const auto row = [&](const RECT& r, LONG y) { return a.bits + size_t(y) * a.stride + size_t(r.left) * 3; };
    const auto fill = [&](const RECT& r) {
        for (LONG y = r.top; y < r.bottom; ++y) {
            BYTE* d = row(r, y);
            for (LONG x = r.left; x < r.right; ++x, d += 3) memcpy(d, bg, 3);
        }
    };

    // Each loop starts over on an empty canvas; otherwise undo the previous frame as it asks.
    if (i == 0) {
        fill({0, 0, LONG(a.width), LONG(a.height)});
    } else if (const Frame& prev = a.frames[i - 1]; prev.disposal == 2) {
        fill(prev.rect);
    } else if (prev.disposal == 3) {
        const size_t bytes = size_t(Width(prev.rect)) * 3;
        for (LONG y = prev.rect.top; y < prev.rect.bottom; ++y) {
            memcpy(row(prev.rect, y), a.saved.data() + size_t(y - prev.rect.top) * bytes, bytes);
        }
    }

    const Frame& f = a.frames[i];
    const size_t bytes = size_t(Width(f.rect)) * 3;
    if (f.disposal == 3) {
        for (LONG y = f.rect.top; y < f.rect.bottom; ++y) {
            memcpy(a.saved.data() + size_t(y - f.rect.top) * bytes, row(f.rect, y), bytes);
        }
    }
    const BYTE* in = a.pixels.get() + f.offset;
    for (LONG y = f.rect.top; y < f.rect.bottom; ++y) {
        BYTE* d = row(f.rect, y);
        for (LONG x = f.rect.left; x < f.rect.right; ++x, ++in, d += 3) {
            if (*in == f.transparent) continue;
            const UINT32 c = f.colors[*in];
            d[0] = BYTE(c);
            d[1] = BYTE(c >> 8);
            d[2] = BYTE(c >> 16);
        }
    }
    return f.delay;
}
