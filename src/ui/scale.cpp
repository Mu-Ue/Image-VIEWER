#include "ui/scale.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <new>
#include <utility>

namespace {

// The source pixels one output pixel covers, along an axis of src pixels shown as out pixels.
// Lengths are in units of 1/out source pixel, so output pixel i spans [i*src, (i+1)*src) and every
// weight is an integer: the first and last pixels may be partly covered, those between weigh out.
// When enlarging, an output pixel covers at most two source pixels.
struct Span {
    UINT first, last;  // inclusive
    UINT firstWeight, lastWeight;  // lastWeight is 0 when first == last
};

Span Cover(UINT i, UINT src, UINT out) {
    const UINT64 a = UINT64(i) * src, b = a + src;
    Span s{UINT(a / out), UINT((b - 1) / out), src, 0};
    if (s.first != s.last) {
        s.firstWeight = UINT(UINT64(s.first + 1) * out - a);
        s.lastWeight = UINT(b - UINT64(s.last) * out);
    }
    return s;
}

// Sums one source row (BGR, starting at source x0) into each column's weighted B, G, R totals.
void SumRow(const BYTE* p, UINT x0, const Span* cols, UINT count, UINT out, UINT32* sums) {
    for (UINT c = 0; c < count; ++c, sums += 3) {
        const Span& s = cols[c];
        const BYTE* q = p + size_t(s.first - x0) * 3;
        UINT32 b = s.firstWeight * q[0], g = s.firstWeight * q[1], r = s.firstWeight * q[2];
        if (s.lastWeight) {
            const BYTE* const last = p + size_t(s.last - x0) * 3;
            UINT32 ib = 0, ig = 0, ir = 0;
            for (q += 3; q != last; q += 3) {
                ib += q[0];
                ig += q[1];
                ir += q[2];
            }
            b += ib * out + s.lastWeight * last[0];
            g += ig * out + s.lastWeight * last[1];
            r += ir * out + s.lastWeight * last[2];
        }
        sums[0] = b;
        sums[1] = g;
        sums[2] = r;
    }
}

// round(sum / area), exactly: the floating-point estimate is off by at most one.
inline UINT32 Average(UINT64 sum, UINT64 area, double inverse) {
    const UINT64 n = sum + area / 2;
    UINT64 q = UINT64(double(n) * inverse);
    if (q * area > n) --q;
    else if ((q + 1) * area <= n) ++q;
    return UINT32(q);
}

constexpr int kBand = 256;  // output rows drawn at a time
constexpr int kChunk = 16;  // output rows a thread takes at a time

// One draw, shared by the threads that compute it.
struct Job {
    const BYTE* bits;
    size_t stride;
    UINT bpp;
    const RGBQUAD* colors;
    UINT srcH, width, height;
    const Span* spans;  // of the visible columns
    UINT cols, x0, x1;  // visible columns; source columns [x0, x1) they cover
    UINT64 area;
    double inverse;
    UINT32* band;  // output rows [b0, b0 + n), relative to the image's top
    int b0, n;
    std::atomic<int> next;  // next chunk of the band to compute
    std::atomic<bool> failed;
};

// Computes chunks of the current band until none are left.
void RunChunks(Job& job) {
    const size_t values = size_t(job.cols) * 3;
    const std::unique_ptr<UINT32[]> sums(new (std::nothrow) UINT32[values * 2]);
    const std::unique_ptr<UINT64[]> acc(new (std::nothrow) UINT64[values]);
    const std::unique_ptr<BYTE[]> expanded(job.bpp <= 8 ? new (std::nothrow) BYTE[size_t(job.x1 - job.x0) * 3]
                                                        : nullptr);
    if (!sums || !acc || (job.bpp <= 8 && !expanded)) {
        job.failed = true;
        return;
    }

    // Source row sy's visible pixels as BGR: palettized rows are expanded first.
    const auto row = [&](UINT sy) -> const BYTE* {
        const BYTE* line = job.bits + sy * job.stride;
        if (job.bpp == 24) return line + size_t(job.x0) * 3;
        BYTE* d = expanded.get();
        const UINT bpp = job.bpp, mask = (1u << bpp) - 1;
        for (UINT sx = job.x0; sx < job.x1; ++sx, d += 3) {
            const UINT bit = sx * bpp;
            const RGBQUAD& q = job.colors[(line[bit / 8] >> (8 - bpp - bit % 8)) & mask];
            d[0] = q.rgbBlue;
            d[1] = q.rgbGreen;
            d[2] = q.rgbRed;
        }
        return expanded.get();
    };

    // The last source row of an output row is often the first of the next, so the latest sums are kept.
    UINT32* cached = sums.get();
    UINT32* scratch = cached + values;
    UINT cachedY = UINT(-1);
    for (int i; (i = job.next++) * kChunk < job.n;) {
        for (int r = i * kChunk, end = std::min(job.n, r + kChunk); r < end; ++r) {
            const Span s = Cover(UINT(job.b0 + r), job.srcH, job.height);
            std::fill_n(acc.get(), values, UINT64(0));
            for (UINT sy = s.first; sy <= s.last; ++sy) {
                const UINT64 w = sy == s.first ? s.firstWeight : sy == s.last ? s.lastWeight : job.height;
                if (sy != cachedY) {
                    SumRow(row(sy), job.x0, job.spans, job.cols, job.width, scratch);
                    std::swap(cached, scratch);
                    cachedY = sy;
                }
                for (size_t k = 0; k < values; ++k) acc[k] += w * cached[k];
            }
            UINT32* out = job.band + size_t(r) * job.cols;
            for (UINT c = 0; c < job.cols; ++c) {
                const UINT64* a = acc.get() + size_t(c) * 3;
                out[c] = Average(a[0], job.area, job.inverse) | Average(a[1], job.area, job.inverse) << 8 |
                         Average(a[2], job.area, job.inverse) << 16;
            }
        }
    }
}

void CALLBACK Work(PTP_CALLBACK_INSTANCE, void* job, PTP_WORK) { RunChunks(*static_cast<Job*>(job)); }

}  // namespace

bool DrawScaled(HDC dc, HDC imageDC, int x, int y, int width, int height, const RECT& clip) {
    DIBSECTION ds{};
    if (GetObjectW(GetCurrentObject(imageDC, OBJ_BITMAP), sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits) {
        return false;
    }
    const UINT bpp = ds.dsBm.bmBitsPixel;
    const UINT srcW = UINT(ds.dsBm.bmWidth), srcH = UINT(ds.dsBm.bmHeight);
    if ((bpp != 24 && bpp != 8 && bpp != 4 && bpp != 1) || width <= 0 || height <= 0) return false;
    RGBQUAD colors[256] = {};
    if (bpp <= 8) GetDIBColorTable(imageDC, 0, 256, colors);

    // The visible output rectangle, relative to the image's top left.
    const int c0 = std::max(0, int(clip.left) - x), c1 = std::min(width, int(clip.right) - x);
    const int r0 = std::max(0, int(clip.top) - y), r1 = std::min(height, int(clip.bottom) - y);
    if (c1 <= c0 || r1 <= r0) return true;
    const UINT cols = UINT(c1 - c0);

    const std::unique_ptr<Span[]> spans(new (std::nothrow) Span[cols]);
    const std::unique_ptr<UINT32[]> band(new (std::nothrow) UINT32[size_t(cols) * kBand]);
    if (!spans || !band) return false;
    for (UINT c = 0; c < cols; ++c) spans[c] = Cover(UINT(c0) + c, srcW, UINT(width));

    Job job{static_cast<const BYTE*>(ds.dsBm.bmBits), size_t(ds.dsBm.bmWidthBytes), bpp, colors, srcH,
            UINT(width), UINT(height), spans.get(), cols, spans[0].first, spans[cols - 1].last + 1,
            UINT64(srcW) * srcH, 1.0 / double(UINT64(srcW) * srcH), band.get()};
    job.failed = false;

    // Each band is split across the thread pool, with this thread helping.
    const PTP_WORK work = CreateThreadpoolWork(Work, &job, nullptr);
    const int helpers = int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) - 1;
    BITMAPINFOHEADER header{sizeof(header), LONG(cols), 0, 1, 32, BI_RGB};
    for (int b0 = r0; b0 < r1 && !job.failed; b0 += kBand) {
        job.b0 = b0;
        job.n = std::min(kBand, r1 - b0);
        job.next = 0;
        for (int i = std::min(helpers, (job.n - 1) / kChunk); work && i > 0; --i) SubmitThreadpoolWork(work);
        RunChunks(job);
        if (work) WaitForThreadpoolWorkCallbacks(work, FALSE);
        header.biHeight = -job.n;  // top-down
        SetDIBitsToDevice(dc, x + c0, y + b0, cols, UINT(job.n), 0, 0, 0, UINT(job.n), band.get(),
                          reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS);
    }
    if (work) CloseThreadpoolWork(work);
    return !job.failed;
}
