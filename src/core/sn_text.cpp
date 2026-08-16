/*
 * BENCsnip - text on the picture. See sn_text.h for why it lives in the core.
 */

#include "sn_text.h"

#include "sn_embed.h"

/* One translation unit defines the implementation, and this is it.
 *
 * Not STBTT_STATIC. It would keep the whole of stb_truetype out of the
 * library's symbol table, and it would also make every function this file does
 * not call an unused static - thirty warnings from a header nobody here is
 * going to edit, in a build that is otherwise clean enough to read. */
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../vendor/stb/stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace sn {

/* ------------------------------------------------------------------ *
 * UTF-8
 *
 * stb_truetype works in codepoints and a caption is typed by a person, so
 * something has to decode. Malformed input yields U+FFFD and moves on by one
 * byte: a text overlay is not the place to reject a file.
 * ------------------------------------------------------------------ */

static uint32_t utf8_next(const std::string &s, size_t *i)
{
    const unsigned char *p = (const unsigned char *)s.data();
    const size_t n = s.size();
    size_t k = *i;

    const unsigned char c = p[k];
    int extra;
    uint32_t cp;

    if (c < 0x80)        { *i = k + 1; return c; }
    else if ((c & 0xe0) == 0xc0) { extra = 1; cp = c & 0x1fu; }
    else if ((c & 0xf0) == 0xe0) { extra = 2; cp = c & 0x0fu; }
    else if ((c & 0xf8) == 0xf0) { extra = 3; cp = c & 0x07u; }
    else                 { *i = k + 1; return 0xfffdu; }

    if (k + (size_t)extra >= n) { *i = k + 1; return 0xfffdu; }

    for (int j = 1; j <= extra; j++) {
        const unsigned char cc = p[k + (size_t)j];
        if ((cc & 0xc0) != 0x80) { *i = k + 1; return 0xfffdu; }
        cp = (cp << 6) | (cc & 0x3fu);
    }
    *i = k + (size_t)extra + 1;
    return cp;
}

/* ------------------------------------------------------------------ *
 * Faces
 *
 * A face is a file read into memory and an stbtt_fontinfo pointing into it.
 * Both are immutable once built, which is what lets the player thread and the
 * exporter thread share one: only the map needs the lock, and only while it
 * is being looked in.
 *
 * Never evicted. A project has a handful of fonts in it and a face is a few
 * hundred kilobytes; a cache that could throw one away would have to answer
 * what happens to the stbtt_fontinfo somebody is holding.
 * ------------------------------------------------------------------ */

namespace {

struct Face {
    std::vector<uint8_t> data;
    stbtt_fontinfo info;
    bool ok = false;
};

std::mutex g_faceLock;
std::map<std::string, Face *> g_faces;   /* "path\0index", or "" for embedded */

Face *build_face(std::vector<uint8_t> bytes, int index)
{
    Face *f = new Face();
    f->data.swap(bytes);

    const int off = stbtt_GetFontOffsetForIndex(f->data.data(), index);
    if (off >= 0 && stbtt_InitFont(&f->info, f->data.data(), off)) f->ok = true;
    return f;
}

std::vector<uint8_t> read_file(const std::string &path)
{
    std::vector<uint8_t> out;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return out;

    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    /* A font is a few hundred kilobytes. Anything claiming to be more than
     * sixty megabytes is not one, and stb_truetype's own warning is that it
     * does no range checking on what it is handed. */
    if (n > 0 && n < 64L * 1024 * 1024) {
        out.resize((size_t)n);
        if (fread(out.data(), 1, (size_t)n, fp) != (size_t)n) out.clear();
    }
    fclose(fp);
    return out;
}

/* The face this style asks for, or the embedded one when it cannot be had.
 * `fellBack` says which happened, for textMissingFont. */
Face *face_for(const TextStyle &st, bool *fellBack)
{
    if (fellBack) *fellBack = false;

    std::lock_guard<std::mutex> lk(g_faceLock);

    if (!st.font.empty()) {
        auto it = g_faces.find(st.font);
        if (it == g_faces.end()) {
            std::vector<uint8_t> bytes = read_file(st.font);
            Face *f = bytes.empty() ? new Face() : build_face(std::move(bytes), 0);
            g_faces[st.font] = f;
            it = g_faces.find(st.font);
        }
        if (it->second->ok) return it->second;
        if (fellBack) *fellBack = true;
    }

    auto it = g_faces.find(std::string());
    if (it == g_faces.end()) {
        std::vector<uint8_t> bytes(SN_FONT_TTF, SN_FONT_TTF + SN_FONT_TTF_LEN);
        g_faces[std::string()] = build_face(std::move(bytes), 0);
        it = g_faces.find(std::string());
    }
    return it->second->ok ? it->second : nullptr;
}

/* ------------------------------------------------------------------ *
 * Layout
 * ------------------------------------------------------------------ */

struct Line {
    std::vector<uint32_t> cp;
    double width = 0;
};

std::vector<Line> lay_out(const Face *f, const TextStyle &st, float scale)
{
    std::vector<Line> lines(1);

    for (size_t i = 0; i < st.text.size();) {
        const uint32_t cp = utf8_next(st.text, &i);
        if (cp == '\n') { lines.push_back(Line()); continue; }
        if (cp == '\r') continue;
        lines.back().cp.push_back(cp);
    }

    for (Line &ln : lines) {
        double w = 0;
        for (size_t i = 0; i < ln.cp.size(); i++) {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f->info, (int)ln.cp[i], &adv, &lsb);
            w += adv * (double)scale;
            if (i + 1 < ln.cp.size())
                w += stbtt_GetCodepointKernAdvance(&f->info, (int)ln.cp[i],
                                                   (int)ln.cp[i + 1]) *
                     (double)scale;
        }
        ln.width = w;
    }
    return lines;
}

/* The size of the text's own box, in pixels, before any outline padding. */
struct Metrics {
    float scale = 0;
    double ascent = 0, lineH = 0, step = 0;
    double w = 0, h = 0;
    std::vector<Line> lines;
    bool ok = false;
};

Metrics measure(const TextStyle &st, int canvasH, bool *fellBack)
{
    Metrics m;

    Face *f = face_for(st, fellBack);
    if (!f || st.text.empty() || st.size <= 0 || canvasH <= 0) return m;

    const double px = st.size * canvasH;
    if (px < 1.0) return m;

    m.scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);

    int a = 0, d = 0, g = 0;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
    m.ascent = a * (double)m.scale;
    m.lineH = (a - d) * (double)m.scale;
    m.step = m.lineH * (st.lineSpacing > 0 ? st.lineSpacing : 1.0);

    m.lines = lay_out(f, st, m.scale);

    for (const Line &ln : m.lines) m.w = std::max(m.w, ln.width);
    m.h = m.lineH + (double)(m.lines.size() - 1) * m.step;
    m.ok = m.w > 0 && m.h > 0;
    return m;
}

} /* namespace */

bool TextStyle::operator==(const TextStyle &o) const
{
    return text == o.text && font == o.font && size == o.size && x == o.x &&
           y == o.y && rotation == o.rotation && fill == o.fill &&
           outline == o.outline && outlineWidth == o.outlineWidth &&
           align == o.align && lineSpacing == o.lineSpacing;
}

bool textMissingFont(const TextStyle &st)
{
    bool fell = false;
    face_for(st, &fell);
    return fell;
}

/* --- how far every pixel is from the nearest bit of letter ---
 *
 * The outline used to be a dilation done the plain way: for each pixel, the
 * most opaque thing within a disc of the outline's radius. That is correct
 * and it is O(pixels x radius squared), and both of those grow with the size
 * of the text - so the cost goes as the fourth power of it. Measured, on one
 * word at 1920x1080: 11 ms at a size of 0.09, 551 ms at 0.25, 8.4 seconds at
 * 0.5, and at 1.0 it did not finish. That is the "it gets stuck and becomes
 * unmodifiable" this replaced: the caption was still being rasterised, once
 * per frame, and nothing else got a turn.
 *
 * A distance transform is O(pixels) whatever the radius, so an outline is now
 * free no matter how thick it is. This is Felzenszwalb and Huttenlocher's:
 * the exact squared Euclidean distance, one pass down the rows and one down
 * the columns, each an O(n) walk of the lower envelope of a set of parabolas.
 */
static void edt_1d(const float *f, float *d, int n, int *v, float *z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -1e20f;
    z[1] = 1e20f;

    for (int q = 1; q < n; q++) {
        float s;
        for (;;) {
            const int p = v[k];
            s = ((f[q] + (float)q * q) - (f[p] + (float)p * p)) / (float)(2 * q - 2 * p);
            if (s > z[k]) break;
            k--;
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e20f;
    }

    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < (float)q) k++;
        const float dx = (float)(q - v[k]);
        d[q] = dx * dx + f[v[k]];
    }
}

/* Squared distance from every pixel to the nearest one that is `inside`. */
static void edt_2d(std::vector<float> &g, int w, int h)
{
    const int n = std::max(w, h);
    std::vector<float> f((size_t)n), d((size_t)n), z((size_t)n + 1);
    std::vector<int> v((size_t)n);

    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) f[(size_t)y] = g[(size_t)y * w + x];
        edt_1d(f.data(), d.data(), h, v.data(), z.data());
        for (int y = 0; y < h; y++) g[(size_t)y * w + x] = d[(size_t)y];
    }
    for (int y = 0; y < h; y++) {
        float *row = &g[(size_t)y * w];
        for (int x = 0; x < w; x++) f[(size_t)x] = row[x];
        edt_1d(f.data(), d.data(), w, v.data(), z.data());
        for (int x = 0; x < w; x++) row[x] = d[(size_t)x];
    }
}

/* ------------------------------------------------------------------ *
 * Building the layer
 * ------------------------------------------------------------------ */

bool buildTextLayer(const TextStyle &st, int w, int h, TextLayer *out)
{
    if (!out) return false;
    out->rgba.clear();
    out->w = out->h = 0;
    out->of = st;
    out->forW = w;
    out->forH = h;

    bool fell = false;
    Metrics m = measure(st, h, &fell);
    if (!m.ok) return false;

    Face *f = face_for(st, nullptr);
    if (!f) return false;

    /* The outline grows the box outwards, so the bitmap is bigger than the
     * text by that much on every side, plus one pixel for the antialiasing at
     * the edge of a glyph that would otherwise be cut off. */
    const double outPx = st.outlineWidth > 0 ? st.outlineWidth * st.size * h : 0.0;
    const int pad = (int)std::ceil(outPx) + 1;

    const int bw = (int)std::ceil(m.w) + 2 * pad;
    const int bh = (int)std::ceil(m.h) + 2 * pad;
    if (bw <= 0 || bh <= 0 || (long)bw * bh > 64L * 1024 * 1024) return false;

    /* One coverage channel first. Colour is decided afterwards, and the
     * outline is grown out of this. */
    std::vector<uint8_t> cov((size_t)bw * bh, 0);
    std::vector<uint8_t> glyph;

    for (size_t li = 0; li < m.lines.size(); li++) {
        const Line &ln = m.lines[li];

        double penX = pad;
        if (st.align == 1) penX += (m.w - ln.width) * 0.5;
        else if (st.align == 2) penX += m.w - ln.width;

        const double baseline = pad + m.ascent + (double)li * m.step;

        for (size_t i = 0; i < ln.cp.size(); i++) {
            const int cp = (int)ln.cp[i];

            int gw = 0, gh = 0, gx = 0, gy = 0;
            const float sub = (float)(penX - std::floor(penX));
            stbtt_GetCodepointBitmapBoxSubpixel(&f->info, cp, m.scale, m.scale, sub, 0,
                                                &gx, &gy, &gw, &gh);
            gw -= gx;
            gh -= gy;

            if (gw > 0 && gh > 0) {
                glyph.assign((size_t)gw * gh, 0);
                stbtt_MakeCodepointBitmapSubpixel(&f->info, glyph.data(), gw, gh, gw,
                                                  m.scale, m.scale, sub, 0, cp);

                const int ox = (int)std::floor(penX) + gx;
                const int oy = (int)std::lround(baseline) + gy;

                /* Taken as a maximum rather than written over what is there.
                 * Kerning and italics put glyphs into each other's boxes, and
                 * a copy would cut a notch out of the letter underneath. */
                for (int yy = 0; yy < gh; yy++) {
                    const int dy = oy + yy;
                    if (dy < 0 || dy >= bh) continue;
                    uint8_t *drow = cov.data() + (size_t)dy * bw;
                    const uint8_t *srow = glyph.data() + (size_t)yy * gw;
                    for (int xx = 0; xx < gw; xx++) {
                        const int dx = ox + xx;
                        if (dx < 0 || dx >= bw) continue;
                        if (srow[xx] > drow[dx]) drow[dx] = srow[xx];
                    }
                }
            }

            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
            penX += adv * (double)m.scale;
            if (i + 1 < ln.cp.size())
                penX += stbtt_GetCodepointKernAdvance(&f->info, cp, (int)ln.cp[i + 1]) *
                        (double)m.scale;
        }
    }

    /* --- the outline ---
     *
     * Everything within the outline's radius of a letter, which is the same
     * thing as everything whose distance to a letter is no more than that.
     * The half pixel either side of the boundary is what keeps the edge from
     * being a staircase: the glyphs are antialiased and an outline drawn round
     * them with a hard edge looks worse than no outline at all.
     */
    std::vector<uint8_t> ring;
    if (outPx > 0.0) {
        std::vector<float> dist((size_t)bw * bh);
        for (size_t i = 0; i < dist.size(); i++) dist[i] = cov[i] >= 128 ? 0.0f : 1e20f;
        edt_2d(dist, bw, bh);

        ring.assign((size_t)bw * bh, 0);
        const float r = (float)outPx;
        for (size_t i = 0; i < ring.size(); i++) {
            const float d = std::sqrt(dist[i]);
            const float t = r + 0.5f - d;
            ring[i] = t <= 0.0f ? 0 : (t >= 1.0f ? 255 : (uint8_t)(t * 255.0f));
        }
    }

    /* --- colour ---
     *
     * The outline underneath at its own alpha, the fill over it at the
     * letter's. Premultiplied nowhere: the blit reads straight alpha, and one
     * convention throughout is worth more than the multiply saved. */
    out->rgba.assign((size_t)bw * bh * 4, 0);

    const int fr = (int)((st.fill >> 24) & 0xff), fg = (int)((st.fill >> 16) & 0xff);
    const int fb = (int)((st.fill >> 8) & 0xff), fa = (int)(st.fill & 0xff);
    const int orr = (int)((st.outline >> 24) & 0xff), og = (int)((st.outline >> 16) & 0xff);
    const int ob = (int)((st.outline >> 8) & 0xff), oa = (int)(st.outline & 0xff);

    for (size_t i = 0; i < (size_t)bw * bh; i++) {
        const int ca = cov[i] * fa / 255;
        const int ra = ring.empty() ? 0 : ring[i] * oa / 255;

        uint8_t *d = out->rgba.data() + i * 4;
        if (ca <= 0 && ra <= 0) continue;

        if (ra > 0) { d[0] = (uint8_t)orr; d[1] = (uint8_t)og; d[2] = (uint8_t)ob; d[3] = (uint8_t)ra; }

        if (ca > 0) {
            /* Straight-alpha over: the fill's colour wins in proportion to
             * its own alpha, and the combined alpha is what you get from
             * putting one over the other. */
            const int da = d[3];
            const int na = ca + da * (255 - ca) / 255;
            if (na > 0) {
                d[0] = (uint8_t)((fr * ca + d[0] * da * (255 - ca) / 255) / na);
                d[1] = (uint8_t)((fg * ca + d[1] * da * (255 - ca) / 255) / na);
                d[2] = (uint8_t)((fb * ca + d[2] * da * (255 - ca) / 255) / na);
                d[3] = (uint8_t)na;
            }
        }
    }

    out->w = bw;
    out->h = bh;
    out->anchorH = (int)std::ceil(m.lineH) + 2 * pad;
    return true;
}

/* ------------------------------------------------------------------ *
 * Placing it
 * ------------------------------------------------------------------ */

namespace {

/* Where the layer's centre sits on the canvas, and the rotation in radians.
 * The free space is split by x and y the way a track's picture is: 0 centres,
 * -1 is hard against the left or top, +1 against the right or bottom. */
void placement(const TextStyle &st, int lw, int lh, int anchorH, int w, int h,
               double *cx, double *cy, double *rad)
{
    /* Across, the whole width is placed in the free space, which is what
     * alignment is about and what somebody dragging it left and right means.
     *
     * Down, only the first line is. The top edge then sits where it would sit
     * if the caption were one line, and everything after the first hangs
     * below it - so adding a line, or opening up the line gap, pushes the
     * rest down rather than sliding the first line up to keep the block
     * centred on the same point. */
    const int ah = anchorH > 0 ? anchorH : lh;

    *cx = (w - lw) * 0.5 * (1.0 + st.x) + lw * 0.5;
    *cy = (h - ah) * 0.5 * (1.0 + st.y) + lh * 0.5;
    *rad = st.rotation * 3.14159265358979323846 / 180.0;
}

} /* namespace */

void blitTextLayer(const TextLayer &layer, const TextStyle &st, uint8_t *rgba, int w,
                   int h, double alpha)
{
    if (!layer.valid() || !rgba || w <= 0 || h <= 0 || alpha <= 0.0) return;

    double cx, cy, rad;
    placement(st, layer.w, layer.h, layer.anchorH, w, h, &cx, &cy, &rad);

    const double cs = std::cos(rad), sn_ = std::sin(rad);
    const int A = (int)std::lround(std::min(1.0, alpha) * 255.0);
    if (A <= 0) return;

    /* The rotated bounding box, so an unrotated caption costs its own area
     * and nothing more. */
    const double hx = layer.w * 0.5, hy = layer.h * 0.5;
    const double ex = std::fabs(hx * cs) + std::fabs(hy * sn_);
    const double ey = std::fabs(hx * sn_) + std::fabs(hy * cs);

    int x0 = (int)std::floor(cx - ex), x1 = (int)std::ceil(cx + ex) + 1;
    int y0 = (int)std::floor(cy - ey), y1 = (int)std::ceil(cy + ey) + 1;
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(w, x1); y1 = std::min(h, y1);

    for (int y = y0; y < y1; y++) {
        uint8_t *drow = rgba + (size_t)y * w * 4;
        for (int x = x0; x < x1; x++) {
            /* Backwards: where in the layer did this canvas pixel come from.
             * Sampling forwards leaves holes wherever the rotation stretches
             * one source pixel across two. */
            const double rx = x + 0.5 - cx, ry = y + 0.5 - cy;
            const double sxf = rx * cs + ry * sn_ + hx;
            const double syf = -rx * sn_ + ry * cs + hy;

            if (sxf < 0 || syf < 0 || sxf >= layer.w || syf >= layer.h) continue;

            /* Bilinear, because a rotated edge sampled at the nearest pixel is
             * a staircase and the outline is what it lands on. */
            const int ix = (int)sxf, iy = (int)syf;
            const double fx = sxf - ix, fy = syf - iy;
            const int ix1 = std::min(ix + 1, layer.w - 1);
            const int iy1 = std::min(iy + 1, layer.h - 1);

            const uint8_t *p00 = layer.rgba.data() + ((size_t)iy * layer.w + ix) * 4;
            const uint8_t *p10 = layer.rgba.data() + ((size_t)iy * layer.w + ix1) * 4;
            const uint8_t *p01 = layer.rgba.data() + ((size_t)iy1 * layer.w + ix) * 4;
            const uint8_t *p11 = layer.rgba.data() + ((size_t)iy1 * layer.w + ix1) * 4;

            double c[4];
            for (int k = 0; k < 4; k++) {
                const double top = p00[k] * (1 - fx) + p10[k] * fx;
                const double bot = p01[k] * (1 - fx) + p11[k] * fx;
                c[k] = top * (1 - fy) + bot * fy;
            }

            int a = (int)std::lround(c[3]) * A / 255;
            if (a <= 0) continue;
            if (a > 255) a = 255;

            uint8_t *d = drow + (size_t)x * 4;
            if (a >= 255) {
                d[0] = (uint8_t)std::lround(c[0]);
                d[1] = (uint8_t)std::lround(c[1]);
                d[2] = (uint8_t)std::lround(c[2]);
            } else {
                for (int k = 0; k < 3; k++)
                    d[k] = (uint8_t)((std::lround(c[k]) * a + d[k] * (255 - a)) / 255);
            }
        }
    }
}

bool drawText(const TextStyle &st, uint8_t *rgba, int w, int h, double alpha)
{
    TextLayer l;
    if (!buildTextLayer(st, w, h, &l)) return false;
    blitTextLayer(l, st, rgba, w, h, alpha);
    return true;
}

bool textBox(const TextStyle &st, int w, int h, double corners[8])
{
    if (!corners || w <= 0 || h <= 0) return false;

    bool fell = false;
    const Metrics m = measure(st, h, &fell);
    if (!m.ok) return false;

    /* The same box buildTextLayer makes, padding included, so what the mouse
     * is offered is what the renderer fills. */
    const double outPx = st.outlineWidth > 0 ? st.outlineWidth * st.size * h : 0.0;
    const int pad = (int)std::ceil(outPx) + 1;
    const double lw = std::ceil(m.w) + 2 * pad;
    const double lh = std::ceil(m.h) + 2 * pad;
    const int ah = (int)std::ceil(m.lineH) + 2 * pad;

    double cx, cy, rad;
    placement(st, (int)lw, (int)lh, ah, w, h, &cx, &cy, &rad);

    const double cs = std::cos(rad), si = std::sin(rad);
    const double hx = lw * 0.5, hy = lh * 0.5;
    const double sx[4] = {-hx, hx, hx, -hx};
    const double sy[4] = {-hy, -hy, hy, hy};

    for (int i = 0; i < 4; i++) {
        corners[i * 2 + 0] = cx + sx[i] * cs - sy[i] * si;
        corners[i * 2 + 1] = cy + sx[i] * si + sy[i] * cs;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * What fonts there are
 * ------------------------------------------------------------------ */

namespace {

/* A font's own idea of its name: the "full name", which is the one with the
 * weight in it - "DejaVu Sans Bold" rather than "DejaVu Sans". Windows-format
 * UTF-16BE first, since practically every font has one, then the Macintosh
 * single-byte entry for the few that do not. */
std::string face_name(const stbtt_fontinfo *info)
{
    int len = 0;
    const char *s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MICROSOFT,
                                            STBTT_MS_EID_UNICODE_BMP, 0x409, 4);
    if (s && len > 1) {
        std::string out;
        for (int i = 0; i + 1 < len; i += 2) {
            const unsigned cp = ((unsigned char)s[i] << 8) | (unsigned char)s[i + 1];
            /* Back to UTF-8. Nothing above the basic plane appears in a font
             * name in practice, and a name is a label rather than data. */
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) {
                out += (char)(0xc0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3f));
            } else {
                out += (char)(0xe0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3f));
                out += (char)(0x80 | (cp & 0x3f));
            }
        }
        return out;
    }

    s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MAC,
                                STBTT_MAC_EID_ROMAN, 0, 4);
    if (s && len > 0) return std::string(s, (size_t)len);
    return std::string();
}

bool ends_with_font_ext(const std::string &n)
{
    if (n.size() < 5) return false;
    std::string e = n.substr(n.size() - 4);
    for (char &c : e) c = (char)tolower((unsigned char)c);
    return e == ".ttf" || e == ".otf" || e == ".ttc";
}

void scan_dir(const std::string &dir, int depth, std::vector<FontEntry> *out);

void add_file(const std::string &path, std::vector<FontEntry> *out)
{
    if (!ends_with_font_ext(path)) return;

    std::vector<uint8_t> bytes = read_file(path);
    if (bytes.empty()) return;

    /* A .ttc holds several faces and each is its own entry in the list; a
     * plain .ttf reports one. */
    int n = stbtt_GetNumberOfFonts(bytes.data());
    if (n < 1) n = 1;
    if (n > 64) n = 64;

    for (int i = 0; i < n; i++) {
        const int off = stbtt_GetFontOffsetForIndex(bytes.data(), i);
        if (off < 0) continue;

        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, bytes.data(), off)) continue;

        FontEntry e;
        e.name = face_name(&info);
        e.path = path;
        e.index = i;
        if (e.name.empty()) continue;
        out->push_back(e);
    }
}

#if defined(_WIN32)

void scan_dir(const std::string &dir, int depth, std::vector<FontEntry> *out)
{
    if (depth > 3) return;

    WIN32_FIND_DATAA fd;
    const std::string pat = dir + "\\*";
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scan_dir(full, depth + 1, out);
        else add_file(full, out);
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

#else

void scan_dir(const std::string &dir, int depth, std::vector<FontEntry> *out)
{
    /* Deep enough for the way these directories are actually arranged -
     * /usr/share/fonts/truetype/dejavu is three - and bounded, because a
     * symlink loop in a font directory should not be the thing that hangs a
     * video editor. */
    if (depth > 3) return;

    DIR *d = opendir(dir.c_str());
    if (!d) return;

    while (struct dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;

        struct stat sb;
        if (stat(full.c_str(), &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) scan_dir(full, depth + 1, out);
        else if (S_ISREG(sb.st_mode)) add_file(full, out);
    }
    closedir(d);
}

#endif

std::vector<std::string> font_dirs()
{
    std::vector<std::string> d;
    const char *home = getenv("HOME");

#if defined(_WIN32)
    if (const char *win = getenv("WINDIR")) d.push_back(std::string(win) + "\\Fonts");
    if (const char *la = getenv("LOCALAPPDATA"))
        d.push_back(std::string(la) + "\\Microsoft\\Windows\\Fonts");
#elif defined(__APPLE__)
    d.push_back("/System/Library/Fonts");
    d.push_back("/System/Library/Fonts/Supplemental");
    d.push_back("/Library/Fonts");
    if (home) d.push_back(std::string(home) + "/Library/Fonts");
#else
    d.push_back("/usr/share/fonts");
    d.push_back("/usr/local/share/fonts");
    if (home) {
        d.push_back(std::string(home) + "/.local/share/fonts");
        d.push_back(std::string(home) + "/.fonts");
    }
#endif
    return d;
}

} /* namespace */

const std::vector<FontEntry> &systemFonts()
{
    static std::vector<FontEntry> list;
    static std::once_flag once;

    std::call_once(once, [] {
        for (const std::string &dir : font_dirs()) scan_dir(dir, 0, &list);

        std::sort(list.begin(), list.end(), [](const FontEntry &a, const FontEntry &b) {
            if (a.name != b.name) return a.name < b.name;
            return a.path < b.path;
        });

        /* The same face installed system-wide and again for one user is one
         * face as far as a menu is concerned. */
        list.erase(std::unique(list.begin(), list.end(),
                               [](const FontEntry &a, const FontEntry &b) {
                                   return a.name == b.name;
                               }),
                   list.end());
    });

    return list;
}

} /* namespace sn */
