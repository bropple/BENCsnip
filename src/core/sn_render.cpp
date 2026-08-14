/*
 * BENCsnip - the timeline as pictures and sound
 */

#include "sn_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sn {

double fadeGain(const Clip &c, double t)
{
    double g = 1.0;
    if (c.fadeIn > 0) {
        double x = (t - c.pos) / c.fadeIn;
        if (x < 1.0) g = std::min(g, std::max(0.0, x));
    }
    if (c.fadeOut > 0) {
        double x = (c.end() - t) / c.fadeOut;
        if (x < 1.0) g = std::min(g, std::max(0.0, x));
    }
    return g;
}

Renderer::~Renderer()
{
    reset();
}

void Renderer::setProject(const Project *p)
{
    if (m_p != p) reset();
    m_p = p;
}

void Renderer::reset()
{
    for (auto &kv : m_open) delete kv.second;
    m_open.clear();
    m_failed.clear();
}

Source *Renderer::source(int itemId)
{
    auto it = m_open.find(itemId);
    if (it != m_open.end()) return it->second;
    if (m_failed.count(itemId)) return nullptr;

    const BinItem *b = m_p ? m_p->item(itemId) : nullptr;
    if (!b || b->missing) { m_failed[itemId] = true; return nullptr; }

    std::string err;
    Source *s = Source::open(b->info.path, &err);
    if (!s) {
        /* Remembered, so a timeline full of clips from one broken file does
         * not try to open it once per frame. */
        m_failed[itemId] = true;
        m_err = err;
        return nullptr;
    }
    m_open[itemId] = s;
    return s;
}

/* ------------------------------------------------------------------ *
 * Video
 * ------------------------------------------------------------------ */

/* Largest w x h box with the source's aspect that fits inside the frame. */
static void fit(int sw, int sh, int fw, int fh, int *ow, int *oh, int *ox, int *oy)
{
    if (sw <= 0 || sh <= 0) { *ow = fw; *oh = fh; *ox = *oy = 0; return; }

    double sa = (double)sw / sh, fa = (double)fw / fh;
    if (sa > fa) { *ow = fw; *oh = (int)std::lround(fw / sa); }
    else         { *oh = fh; *ow = (int)std::lround(fh * sa); }

    /* Odd sizes are legal here - this is a preview buffer, not an encoder
     * input - but an off-by-one that leaves a one-pixel seam at the bottom is
     * not, so clamp. */
    if (*ow > fw) *ow = fw;
    if (*oh > fh) *oh = fh;
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
    *ox = (fw - *ow) / 2;
    *oy = (fh - *oh) / 2;
}

bool Renderer::videoAt(double t, int w, int h, VideoFrame *out)
{
    if (w <= 0 || h <= 0) return false;

    const size_t bytes = (size_t)w * h * 4;
    if (m_canvas.size() != bytes) m_canvas.assign(bytes, 0);

    /* Opaque black underneath everything, so a fade at the bottom of the
     * stack fades to black rather than to whatever the last frame was. */
    uint32_t *canvas = (uint32_t *)m_canvas.data();
    const uint32_t black = 0xff000000u;
    for (size_t i = 0; i < bytes / 4; i++) canvas[i] = black;

    out->w = w;
    out->h = h;
    out->pts = t;

    bool any = false;
    if (!m_p) { out->rgba = m_canvas; return false; }

    /* Bottom to top: tracks are stored top-first, so this walks backwards.
     * Only video tracks take part. */
    for (int ti = (int)m_p->tracks.size() - 1; ti >= 0; ti--) {
        const Track &tr = m_p->tracks[ti];
        if (tr.kind != TRACK_VIDEO || tr.muted) continue;

        const Clip *c = tr.at(t);
        if (!c) continue;

        const BinItem *b = m_p->item(c->source);
        if (!b || !b->info.hasVideo) continue;

        Source *s = source(c->source);
        if (!s || !s->hasVideo()) continue;

        int fw, fh, ox, oy;
        fit(b->info.dispW(), b->info.dispH(), w, h, &fw, &fh, &ox, &oy);

        if (!s->frameAt(c->srcAt(t), &m_layer, fw, fh)) continue;
        if (!m_layer.valid()) continue;

        const double g = fadeGain(*c, t);
        if (g <= 0.0) { any = true; continue; }

        const uint8_t *src = m_layer.rgba.data();
        const int lw = std::min(m_layer.w, w - ox);
        const int lh = std::min(m_layer.h, h - oy);

        if (g >= 0.999) {
            for (int y = 0; y < lh; y++)
                std::memcpy(m_canvas.data() + (((size_t)(y + oy) * w) + ox) * 4,
                            src + (size_t)y * m_layer.w * 4, (size_t)lw * 4);
        } else {
            const int a = (int)std::lround(g * 255.0);
            for (int y = 0; y < lh; y++) {
                uint8_t *d = m_canvas.data() + (((size_t)(y + oy) * w) + ox) * 4;
                const uint8_t *ss = src + (size_t)y * m_layer.w * 4;
                for (int x = 0; x < lw * 4; x += 4) {
                    d[x + 0] = (uint8_t)((ss[x + 0] * a + d[x + 0] * (255 - a)) / 255);
                    d[x + 1] = (uint8_t)((ss[x + 1] * a + d[x + 1] * (255 - a)) / 255);
                    d[x + 2] = (uint8_t)((ss[x + 2] * a + d[x + 2] * (255 - a)) / 255);
                    d[x + 3] = 255;
                }
            }
        }
        any = true;
    }

    out->rgba = m_canvas;
    return any;
}

/* ------------------------------------------------------------------ *
 * Audio
 * ------------------------------------------------------------------ */

bool Renderer::hasAudioAt(double t, double dur) const
{
    if (!m_p) return false;
    for (const Track &tr : m_p->tracks) {
        if (tr.kind != TRACK_AUDIO || tr.muted) continue;
        for (const Clip &c : tr.clips)
            if (c.pos < t + dur && t < c.end() && !c.muted) return true;
    }
    return false;
}

void Renderer::audioAt(double t, int frames, float *dst)
{
    std::memset(dst, 0, (size_t)frames * CHANS * sizeof(float));
    if (!m_p || frames <= 0) return;

    if ((int)m_mix.size() < frames * CHANS) m_mix.resize((size_t)frames * CHANS);

    const double bs = t;
    const double be = t + frames / (double)RATE;

    for (const Track &tr : m_p->tracks) {
        if (tr.kind != TRACK_AUDIO || tr.muted) continue;

        for (const Clip &c : tr.clips) {
            if (c.muted || c.gain == 0.0) continue;
            const double a = std::max(bs, c.pos);
            const double b = std::min(be, c.end());
            if (b <= a + 1e-9) continue;

            const BinItem *bi = m_p->item(c.source);
            if (!bi || !bi->info.hasAudio) continue;
            Source *s = source(c.source);
            if (!s || !s->hasAudio()) continue;

            int off = (int)std::lround((a - bs) * RATE);
            int n = (int)std::lround((b - a) * RATE);
            if (off < 0) off = 0;
            if (n <= 0) continue;
            if (off + n > frames) n = frames - off;
            if (n <= 0) continue;

            s->audioAt(c.srcAt(a), n, m_mix.data());

            /* Fades are evaluated per sample rather than per block: a fade
             * shorter than a block would otherwise be a step, and a step in
             * a gain is a click. */
            const bool ramp = c.fadeIn > 0 || c.fadeOut > 0;
            float *o = dst + (size_t)off * CHANS;

            if (!ramp) {
                const float g = (float)c.gain;
                for (int i = 0; i < n * CHANS; i++) o[i] += m_mix[i] * g;
            } else {
                for (int i = 0; i < n; i++) {
                    const double ts = a + i / (double)RATE;
                    const float g = (float)(c.gain * fadeGain(c, ts));
                    o[i * CHANS + 0] += m_mix[i * CHANS + 0] * g;
                    o[i * CHANS + 1] += m_mix[i * CHANS + 1] * g;
                }
            }
        }
    }

    /* Two clips at full level sum past 1.0. Clipping here rather than letting
     * it wrap in the encoder means an overloaded mix sounds like an overloaded
     * mix instead of like a fault. */
    for (int i = 0; i < frames * CHANS; i++) {
        if (dst[i] > 1.0f) dst[i] = 1.0f;
        else if (dst[i] < -1.0f) dst[i] = -1.0f;
    }
}

} /* namespace sn */
