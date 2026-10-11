#include "ui/scale.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "platform/pages.h"

namespace {

// The source pixels one output pixel covers, along an axis of src pixels shown as out pixels.
// Lengths are in units of 1/out source pixel, so output pixel i spans [i*src, (i+1)*src) and every
// weight is an integer: the first and last pixels may be partly covered, those between weigh out.
// When enlarging, an output pixel covers at most two source pixels.
struct Span {
    UINT first, last;  // inclusive
    UINT firstWeight, lastWeight;  // lastWeight is 0 when first == last

    bool operator==(const Span&) const = default;
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
// When enlarging, neighboring columns often cover the same source pixel, so their sums are copied.
void SumRow(const BYTE* p, UINT x0, const Span* cols, UINT count, UINT out, UINT32* sums) {
    for (UINT c = 0; c < count; ++c, sums += 3) {
        const Span& s = cols[c];
        if (c && s == cols[c - 1]) {
            std::copy_n(sums - 3, 3, sums);
            continue;
        }
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
    BYTE* scratch;  // one slot of slotBytes per thread computing the band
    size_t slotBytes;
    std::atomic<int> slot;  // next free slot
};

// Bytes of scratch one thread needs: running totals, two rows of column sums and, for palettized
// images, a row expanded to BGR.
size_t SlotBytes(UINT cols, UINT bpp, UINT x0, UINT x1) {
    const size_t values = size_t(cols) * 3;
    const size_t bytes = values * sizeof(UINT64) + values * 2 * sizeof(UINT32) + (bpp <= 8 ? size_t(x1 - x0) * 3 : 0);
    return (bytes + 63) & ~size_t(63);
}

// Computes chunks of the current band until none are left.
void RunChunks(Job& job) {
    const size_t values = size_t(job.cols) * 3;
    BYTE* const mine = job.scratch + size_t(job.slot++) * job.slotBytes;
    UINT64* const acc = reinterpret_cast<UINT64*>(mine);
    UINT32* const sums = reinterpret_cast<UINT32*>(acc + values);
    BYTE* const expanded = reinterpret_cast<BYTE*>(sums + values * 2);

    // Source row sy's visible pixels as BGR: palettized rows are expanded first.
    const auto row = [&](UINT sy) -> const BYTE* {
        const BYTE* line = job.bits + sy * job.stride;
        if (job.bpp == 24) return line + size_t(job.x0) * 3;
        BYTE* d = expanded;
        const UINT bpp = job.bpp, mask = (1u << bpp) - 1;
        for (UINT sx = job.x0; sx < job.x1; ++sx, d += 3) {
            const UINT bit = sx * bpp;
            const RGBQUAD& q = job.colors[(line[bit / 8] >> (8 - bpp - bit % 8)) & mask];
            d[0] = q.rgbBlue;
            d[1] = q.rgbGreen;
            d[2] = q.rgbRed;
        }
        return expanded;
    };

    // The last source row of an output row is often the first of the next, so the latest sums are kept.
    UINT32* cached = sums;
    UINT32* scratch = cached + values;
    UINT cachedY = UINT(-1);
    for (int i; (i = job.next++) * kChunk < job.n;) {
        Span previous{};
        for (int r = i * kChunk, end = std::min(job.n, r + kChunk); r < end; ++r) {
            const Span s = Cover(UINT(job.b0 + r), job.srcH, job.height);
            UINT32* out = job.band + size_t(r) * job.cols;
            // When enlarging, neighboring rows often cover the same source rows and come out the same.
            if (r != i * kChunk && s == previous) {
                std::copy_n(out - job.cols, job.cols, out);
                continue;
            }
            previous = s;
            std::fill_n(acc, values, UINT64(0));
            for (UINT sy = s.first; sy <= s.last; ++sy) {
                const UINT64 w = sy == s.first ? s.firstWeight : sy == s.last ? s.lastWeight : job.height;
                if (sy != cachedY) {
                    SumRow(row(sy), job.x0, job.spans, job.cols, job.width, scratch);
                    std::swap(cached, scratch);
                    cachedY = sy;
                }
                for (size_t k = 0; k < values; ++k) acc[k] += w * cached[k];
            }
            for (UINT c = 0; c < job.cols; ++c) {
                if (c && job.spans[c] == job.spans[c - 1]) {
                    out[c] = out[c - 1];
                    continue;
                }
                const UINT64* a = acc + size_t(c) * 3;
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

    // At 100% every pixel covers exactly one image pixel.
    if (UINT(width) == srcW && UINT(height) == srcH) {
        return BitBlt(dc, x + c0, y + r0, c1 - c0, r1 - r0, imageDC, c0, r0, SRCCOPY);
    }
    const UINT cols = UINT(c1 - c0);

    // Each band is split across the thread pool, with this thread helping.
    const int helpers = std::min(int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) - 1, (kBand - 1) / kChunk);
    const UINT x0 = Cover(UINT(c0), srcW, UINT(width)).first, x1 = Cover(UINT(c1 - 1), srcW, UINT(width)).last + 1;
    const size_t slotBytes = SlotBytes(cols, bpp, x0, x1);
    const Pages<Span> spans = AllocPages<Span>(cols);
    const Pages<UINT32> band = AllocPages<UINT32>(size_t(cols) * kBand);
    const Pages<BYTE> scratch = AllocPages<BYTE>(slotBytes * (helpers + 1));
    if (!spans || !band || !scratch) return false;
    for (UINT c = 0; c < cols; ++c) spans[c] = Cover(UINT(c0) + c, srcW, UINT(width));

    Job job{static_cast<const BYTE*>(ds.dsBm.bmBits), size_t(ds.dsBm.bmWidthBytes), bpp, colors, srcH,
            UINT(width), UINT(height), spans.get(), cols, x0, x1,
            UINT64(srcW) * srcH, 1.0 / double(UINT64(srcW) * srcH), band.get()};
    job.scratch = scratch.get();
    job.slotBytes = slotBytes;

    const PTP_WORK work = CreateThreadpoolWork(Work, &job, nullptr);
    BITMAPINFOHEADER header{sizeof(header), LONG(cols), 0, 1, 32, BI_RGB};
    for (int b0 = r0; b0 < r1; b0 += kBand) {
        job.b0 = b0;
        job.n = std::min(kBand, r1 - b0);
        job.next = 0;
        job.slot = 0;  // every callback of the previous band has finished
        for (int i = std::min(helpers, (job.n - 1) / kChunk); work && i > 0; --i) SubmitThreadpoolWork(work);
        RunChunks(job);
        if (work) WaitForThreadpoolWorkCallbacks(work, FALSE);
        header.biHeight = -job.n;  // top-down
        SetDIBitsToDevice(dc, x + c0, y + b0, cols, UINT(job.n), 0, 0, 0, UINT(job.n), band.get(),
                          reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS);
    }
    if (work) CloseThreadpoolWork(work);
    return true;
}
