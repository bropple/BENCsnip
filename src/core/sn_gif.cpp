/*
 * BENCsnip - the palette a GIF needs. See sn_gif.h for why it is here.
 */

#include "sn_gif.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace sn {

namespace {

/* A box in colour space, and the pixels inside it. Median cut splits the box
 * with the widest spread at its median, again and again, until there are as
 * many boxes as there are palette entries. Each box then contributes the
 * average of what fell in it.
 *
 * The alternative most tools reach for is a k-means pass over the same data,
 * which is better by a margin nobody watching a GIF has ever noticed and
 * costs twenty times as much. */
struct Box {
    size_t begin = 0, end = 0;    /* half-open range into the pixel array */
    uint8_t lo[3] = {255, 255, 255};
    uint8_t hi[3] = {0, 0, 0};

    size_t count() const { return end - begin; }
    int widest() const
    {
        const int r = hi[0] - lo[0], g = hi[1] - lo[1], b = hi[2] - lo[2];
        if (g >= r && g >= b) return 1;
        return r >= b ? 0 : 2;
    }
    int spread() const
    {
        const int c = widest();
        return hi[c] - lo[c];
    }
};

void measure(Box &b, const std::vector<uint32_t> &px)
{
    b.lo[0] = b.lo[1] = b.lo[2] = 255;
    b.hi[0] = b.hi[1] = b.hi[2] = 0;
    for (size_t i = b.begin; i < b.end; i++) {
        const uint32_t v = px[i];
        const uint8_t c[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        for (int k = 0; k < 3; k++) {
            if (c[k] < b.lo[k]) b.lo[k] = c[k];
            if (c[k] > b.hi[k]) b.hi[k] = c[k];
        }
    }
}

} /* namespace */

void buildPalette(const uint8_t *rgba, size_t pixels, int want, Palette *out)
{
    static std::atomic<uint32_t> serial{1};
    out->serial = serial.fetch_add(1);

    out->n = 0;
    if (want < 2) want = 2;
    if (want > 256) want = 256;
    if (!rgba || pixels == 0) {
        out->n = 1;
        out->rgb[0][0] = out->rgb[0][1] = out->rgb[0][2] = 0;
        return;
    }

    /* Every pixel packed into one integer, so the sort below moves 4 bytes
     * rather than a struct, and identical colours end up adjacent - which is
     * most of a photograph and nearly all of a screen recording. */
    std::vector<uint32_t> px;
    px.reserve(pixels);
    for (size_t i = 0; i < pixels; i++) {
        const uint8_t *p = rgba + i * 4;
        px.push_back(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
    }

    std::vector<Box> boxes;
    Box first;
    first.begin = 0;
    first.end = px.size();
    measure(first, px);
    boxes.push_back(first);

    while ((int)boxes.size() < want) {
        /* Split whichever box covers the widest range of one channel. A box
         * holding one colour cannot be split at all, which is how a flat
         * cartoon ends up with a palette of exactly as many entries as it has
         * colours instead of 256 near-duplicates. */
        int pick = -1, best = 0;
        for (size_t i = 0; i < boxes.size(); i++) {
            if (boxes[i].count() < 2) continue;
            const int s = boxes[i].spread();
            if (s > best) { best = s; pick = (int)i; }
        }
        if (pick < 0) break;

        Box &b = boxes[pick];
        const int ch = b.widest();
        const int shift = ch == 0 ? 16 : (ch == 1 ? 8 : 0);

        std::sort(px.begin() + b.begin, px.begin() + b.end,
                  [shift](uint32_t l, uint32_t r) {
                      return ((l >> shift) & 0xff) < ((r >> shift) & 0xff);
                  });

        const size_t mid = b.begin + b.count() / 2;
        Box right;
        right.begin = mid;
        right.end = b.end;
        b.end = mid;
        measure(b, px);
        measure(right, px);
        boxes.push_back(right);
    }

    for (const Box &b : boxes) {
        if (b.count() == 0) continue;
        uint64_t sum[3] = {0, 0, 0};
        for (size_t i = b.begin; i < b.end; i++) {
            const uint32_t v = px[i];
            sum[0] += (v >> 16) & 0xff;
            sum[1] += (v >> 8) & 0xff;
            sum[2] += v & 0xff;
        }
        const size_t n = b.count();
        uint8_t *e = out->rgb[out->n++];
        e[0] = (uint8_t)(sum[0] / n);
        e[1] = (uint8_t)(sum[1] / n);
        e[2] = (uint8_t)(sum[2] / n);
        if (out->n >= 256) break;
    }

    if (out->n == 0) {
        out->n = 1;
        out->rgb[0][0] = out->rgb[0][1] = out->rgb[0][2] = 0;
    }
}

void quantise(const uint8_t *rgba, int w, int h, const Palette &pal, uint8_t *indices,
              int indexStride, bool dither)
{
    if (pal.n <= 0 || w <= 0 || h <= 0) return;

    /* Nearest-colour lookups, cached on the top five bits of each channel.
     * Thirty-two thousand entries, filled as they are asked for: without it
     * every pixel is a 256-entry search, which at 1280x720x30 is a minute of
     * arithmetic per second of GIF. */
    static thread_local std::vector<int16_t> cache;
    static thread_local uint32_t cachedFor = 0;

    if (cachedFor != pal.serial || cache.size() != 32768) {
        cache.assign(32768, -1);
        cachedFor = pal.serial;
    }

    /* The classic 8x8 ordered matrix, scaled to the average gap between
     * palette entries: a fine palette needs a small nudge, a coarse one a
     * large one, and a fixed amount would either do nothing or add visible
     * texture to a picture that did not need it. */
    static const int bayer[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};

    const int step = dither ? std::max(2, 160 / std::max(1, pal.n / 8)) : 0;

    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgba + (size_t)y * w * 4;
        uint8_t *dst = indices + (size_t)y * indexStride;

        for (int x = 0; x < w; x++, src += 4) {
            int c[3] = {src[0], src[1], src[2]};

            if (step) {
                const int nudge = (bayer[y & 7][x & 7] - 32) * step / 64;
                for (int k = 0; k < 3; k++)
                    c[k] = std::min(255, std::max(0, c[k] + nudge));
            }

            const int key = ((c[0] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[2] >> 3);
            int idx = cache[key];

            if (idx < 0) {
                int bestD = 1 << 30;
                idx = 0;
                for (int i = 0; i < pal.n; i++) {
                    /* Weighted so that green counts for more than blue, which
                     * is roughly how an eye reads distance between colours.
                     * Plain Euclidean picks visibly wrong entries in skin
                     * tones. */
                    const int dr = c[0] - pal.rgb[i][0];
                    const int dg = c[1] - pal.rgb[i][1];
                    const int db = c[2] - pal.rgb[i][2];
                    const int d = dr * dr * 2 + dg * dg * 4 + db * db;
                    if (d < bestD) { bestD = d; idx = i; }
                }
                cache[key] = (int16_t)idx;
            }

            dst[x] = (uint8_t)idx;
        }
    }
}

} /* namespace sn */
