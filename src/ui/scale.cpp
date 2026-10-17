#include "ui/scale.h"

#include <algorithm>
#include <atomic>
#include <utility>

#if defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#endif

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

// Adds w times each of n bytes to sums. Every weight fits in 16 bits for bitmaps up to 65535 pixels tall,
// where SSE2's 16-bit multiplies give the full 32-bit products, 16 bytes at a time.
void AddRow(const BYTE* p, size_t n, UINT32 w, UINT32* sums) {
    size_t i = 0;
#if defined(_M_X64) || defined(_M_IX86)
    if (w <= 0xFFFF) {
        const __m128i weight = _mm_set1_epi16(short(w)), zero = _mm_setzero_si128();
        for (; i + 16 <= n; i += 16) {
            const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
            __m128i* const s = reinterpret_cast<__m128i*>(sums + i);
            for (int half = 0; half < 2; ++half) {
                const __m128i v = half ? _mm_unpackhi_epi8(bytes, zero) : _mm_unpacklo_epi8(bytes, zero);
                const __m128i lo = _mm_mullo_epi16(v, weight), hi = _mm_mulhi_epu16(v, weight);
                __m128i* const d = s + half * 2;
                _mm_storeu_si128(d, _mm_add_epi32(_mm_loadu_si128(d), _mm_unpacklo_epi16(lo, hi)));
                _mm_storeu_si128(d + 1, _mm_add_epi32(_mm_loadu_si128(d + 1), _mm_unpackhi_epi16(lo, hi)));
            }
        }
    }
#endif
    for (; i < n; ++i) sums[i] += w * p[i];
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
constexpr UINT64 kMinParallelPixels = 1 << 16;  // smaller bands are drawn by the calling thread alone

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

// Bytes of scratch one thread needs: a row of B, G, R totals of the source rows an output row covers and,
// for palettized images, a row expanded to BGR.
size_t SlotBytes(UINT bpp, UINT x0, UINT x1) {
    const size_t values = size_t(x1 - x0) * 3;
    const size_t bytes = values * sizeof(UINT32) + (bpp <= 8 ? values : 0);
    return (bytes + 63) & ~size_t(63);
}

// Computes chunks of the current band until none are left. Each output row first adds up the source rows it
// covers, weighted, into one row of totals (which vectorizes), then sums that row across each column, so
// the horizontal work is done once per output row rather than once per source row.
void RunChunks(Job& job) {
    const size_t values = size_t(job.x1 - job.x0) * 3;
    BYTE* const mine = job.scratch + size_t(job.slot++) * job.slotBytes;
    UINT32* const sums = reinterpret_cast<UINT32*>(mine);
    BYTE* const expanded = reinterpret_cast<BYTE*>(sums + values);

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
            // Totals stay below 2^32: the weights of an output row's source rows add up to the source height.
            std::fill_n(sums, values, 0u);
            for (UINT sy = s.first; sy <= s.last; ++sy) {
                const UINT32 w = sy == s.first ? s.firstWeight : sy == s.last ? s.lastWeight : job.height;
                AddRow(row(sy), values, w, sums);
            }
            // When enlarging, neighboring columns often cover the same source pixels and come out the same.
            for (UINT c = 0; c < job.cols; ++c) {
                const Span& x = job.spans[c];
                if (c && x == job.spans[c - 1]) {
                    out[c] = out[c - 1];
                    continue;
                }
                const UINT32* q = sums + size_t(x.first - job.x0) * 3;
                UINT64 b = UINT64(x.firstWeight) * q[0], g = UINT64(x.firstWeight) * q[1];
                UINT64 red = UINT64(x.firstWeight) * q[2];
                if (x.lastWeight) {
                    const UINT32* const last = sums + size_t(x.last - job.x0) * 3;
                    UINT64 ib = 0, ig = 0, ir = 0;
                    for (q += 3; q != last; q += 3) {
                        ib += q[0];
                        ig += q[1];
                        ir += q[2];
                    }
                    b += ib * job.width + UINT64(x.lastWeight) * last[0];
                    g += ig * job.width + UINT64(x.lastWeight) * last[1];
                    red += ir * job.width + UINT64(x.lastWeight) * last[2];
                }
                out[c] = Average(b, job.area, job.inverse) | Average(g, job.area, job.inverse) << 8 |
                         Average(red, job.area, job.inverse) << 16;
            }
        }
    }
}

// The helpers' own pool, capped at the number of helpers.
PTP_POOL pool;
TP_CALLBACK_ENVIRON environment;

void CALLBACK Work(PTP_CALLBACK_INSTANCE, void* job, PTP_WORK) {
    RunChunks(*static_cast<Job*>(job));
}

// Starts the pool if needed, returning false if there is none.
bool StartPool(int threads) {
    if (pool) return true;
    if (threads <= 0 || !(pool = CreateThreadpool(nullptr))) return false;
    SetThreadpoolThreadMaximum(pool, DWORD(threads));
    InitializeThreadpoolEnvironment(&environment);
    SetThreadpoolCallbackPool(&environment, pool);
    return true;
}

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
    const size_t slotBytes = SlotBytes(bpp, x0, x1);
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

    const PTP_WORK work = StartPool(helpers) ? CreateThreadpoolWork(Work, &job, &environment) : nullptr;
    BITMAPINFOHEADER header{sizeof(header), LONG(cols), 0, 1, 32, BI_RGB};
    for (int b0 = r0; b0 < r1; b0 += kBand) {
        job.b0 = b0;
        job.n = std::min(kBand, r1 - b0);
        job.next = 0;
        job.slot = 0;  // every callback of the previous band has finished
        // A thin strip, as a pan uncovers, is drawn sooner alone than by waking the helpers.
        const UINT top = Cover(UINT(b0), srcH, UINT(height)).first;
        const UINT srcRows = Cover(UINT(b0 + job.n - 1), srcH, UINT(height)).last + 1 - top;
        const bool parallel = std::max(UINT64(cols) * job.n, UINT64(x1 - x0) * srcRows) >= kMinParallelPixels;
        for (int i = parallel ? std::min(helpers, (job.n - 1) / kChunk) : 0; work && i > 0; --i) {
            SubmitThreadpoolWork(work);
        }
        RunChunks(job);
        if (work) WaitForThreadpoolWorkCallbacks(work, FALSE);
        header.biHeight = -job.n;  // top-down
        SetDIBitsToDevice(dc, x + c0, y + b0, cols, UINT(job.n), 0, 0, 0, UINT(job.n), band.get(),
                          reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS);
    }
    if (work) CloseThreadpoolWork(work);
    return true;
}
