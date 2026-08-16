/*
 * BENCsnip - the timeline as pictures and sound
 */

#include "sn_render.h"

#include "sn_text.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sn {

double fxGain(const Track &t, double at)
{
    const double g = t.fxAt(at);
    return g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
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
    m_layers.clear();
    m_text.clear();
}

Source *Renderer::source(int itemId, int channel)
{
    /* One number for the pair. Channel counts are small and bin ids are
     * sequential, so this is a key rather than a hash and cannot collide. */
    const int key = itemId * 64 + (channel < 0 ? 0 : (channel % 63) + 1);

    auto it = m_open.find(key);
    if (it != m_open.end()) return it->second;
    if (m_failed.count(key)) return nullptr;

    const BinItem *b = m_p ? m_p->item(itemId) : nullptr;
    if (!b || b->missing) { m_failed[key] = true; return nullptr; }

    std::string err;
    Source *s = Source::open(b->info.path, &err, channel);
    if (!s) {
        /* Remembered, so a timeline full of clips from one broken file does
         * not try to open it once per frame. */
        m_failed[key] = true;
        m_err = err;
        return nullptr;
    }
    m_open[key] = s;
    return s;
}

/* ------------------------------------------------------------------ *
 * Video
 * ------------------------------------------------------------------ */

bool Renderer::videoAt(double t, int w, int h, VideoFrame *out)
{
    if (w <= 0 || h <= 0) return false;

    /* The end of the timeline holds its last frame rather than going black.
     *
     * A clip covers [pos, end), so at exactly `end` nothing covers anything
     * and the canvas is the black it was cleared to. That is the moment the
     * playhead lands on when playback stops and when End is pressed - so
     * finishing a project made the picture vanish, which reads as the last
     * clip having been lost rather than as the timeline having run out.
     *
     * Half a frame back is enough to be inside the last clip and too little
     * to be a different picture. It is done here rather than in the preview
     * so that the exporter's last frame is the same one: a render that ends
     * on black would put that black in the file. */
    const double dur = m_p ? m_p->duration() : 0.0;
    if (dur > 0.0 && t >= dur) t = dur - 0.001;

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

    /* A deleted track leaves its buffer behind - the project is handed over
     * whole rather than as a list of changes, so there is nothing to tell us
     * it went. Dropping the lot when there are more buffers than tracks costs
     * one frame of scaling and bounds what the map can hold. */
    if (m_layers.size() > m_p->tracks.size()) m_layers.clear();

    /* Front to back is bottom to top of the list: the top row is the back of
     * the picture, so it is drawn first and everything else lands on top of
     * it. That is the opposite of what most editors do and it is what this
     * one was asked for - see the note on the track operations in
     * sn_timeline.h. */
    for (size_t ti = 0; ti < m_p->tracks.size(); ti++) {
        const Track &tr = m_p->tracks[ti];
        if (!visualTrack(tr.kind) || tr.muted) continue;

        const Clip *c = tr.at(t);
        if (!c) continue;

        /* Text goes on in list order with everything else, which is what
         * makes a caption something you can put behind one layer and in front
         * of another rather than a thing that is always on top. */
        if (tr.kind == TRACK_TEXT) {
            const double g = fxGain(tr, t);
            if (g > 0.0 && !c->muted && !c->text.text.empty()) {
                TextLayer &tl = m_text[tr.id];
                if (!tl.matches(c->text, w, h)) buildTextLayer(c->text, w, h, &tl);
                if (tl.valid()) {
                    blitTextLayer(tl, c->text, m_canvas.data(), w, h, g);
                    any = true;
                }
            }
            continue;
        }

        const BinItem *b = m_p->item(c->source);
        if (!b || !b->info.hasVideo) continue;

        Source *s = source(c->source);
        if (!s || !s->hasVideo()) continue;

        /* --- where this layer goes ---
         *
         * trackLayerRect decides, here and in the preview both, so what you
         * drag is what gets written. It used to be worked out twice, once
         * each side, and the two already disagreed by up to a pixel because
         * this one rounded to integers on the way through and the other did
         * not. */
        const LayerRect lr =
            trackLayerRect(tr, b->info.dispW(), b->info.dispH(), w, h);

        const int fw = std::max(1, (int)std::lround(lr.w));
        const int fh = std::max(1, (int)std::lround(lr.h));
        const int ox = (int)std::lround(lr.x);
        const int oy = (int)std::lround(lr.y);

        /* The decoder is asked for whatever size makes the *kept* part come
         * out at fw x fh, and the crop is then a sub-rectangle of it. Scaling
         * the whole frame and cutting afterwards is the same picture for one
         * extra copy, and this way swscale does the work. */
        const double keepX = std::max(0.02, 1.0 - tr.cropL - tr.cropR);
        const double keepY = std::max(0.02, 1.0 - tr.cropT - tr.cropB);
        const int fullW = std::max(1, (int)std::lround(fw / keepX));
        const int fullH = std::max(1, (int)std::lround(fh / keepY));

        /* This track's own buffer, kept from frame to frame so the source
         * can skip the scale when it is being asked for a picture that is
         * already in there. */
        VideoFrame &layer = m_layers[tr.id];

        /* A decode that comes back with nothing keeps whatever this track had
         * last, rather than dropping the layer for a frame.
         *
         * Dropping it is a hole: the layers behind show through and the
         * picture blinks, which for one frame out of sixty reads as a fault
         * in the footage rather than as a fault here. It is what a wrapped
         * GIF used to do at some of its loop points - see Clip::srcAt, where
         * the cause was - and holding the previous picture would have made
         * that invisible instead of merely rare, which is the argument for
         * doing it whatever the cause turns out to be next time.
         *
         * A track that has never decoded anything has nothing to hold, and
         * that one really is skipped. */
        if (!s->frameAt(c->srcAt(t), &layer, fullW, fullH) && !layer.valid()) continue;
        if (!layer.valid()) continue;

        const int cx = (int)std::lround(tr.cropL * layer.w);
        const int cy = (int)std::lround(tr.cropT * layer.h);

        const double g = fxGain(tr, t);
        if (g <= 0.0) { any = true; continue; }

        /* Clipped against the canvas on all four sides: a track pushed off
         * the edge is a legal thing to ask for, and it must not be a write
         * past the end of the buffer. */
        int x0 = std::max(0, ox), y0 = std::max(0, oy);
        int x1 = std::min(w, ox + fw), y1 = std::min(h, oy + fh);
        if (x1 <= x0 || y1 <= y0) { any = true; continue; }

        const int fade = (int)std::lround(std::min(1.0, g) * 255.0);

        /* A solid picture at full level, with every pixel of the row coming
         * from somewhere inside the source, is a copy - no alpha to read, no
         * blend to compute, nothing to decide per pixel. That is what almost
         * every frame of ordinary video is, and it is around three times
         * quicker than the general path below. The moment a layer can be
         * see-through, or is fading, or hangs off the side, it takes the
         * general path instead. */
        const int rsx0 = cx + (x0 - ox), rsx1 = cx + (x1 - ox);

        /* A left-to-right mirror reads each row backwards, which a memcpy
         * cannot do; it takes the general path instead. Top to bottom only
         * changes which row is read, so that one keeps the fast path. */
        const bool solid = fade >= 255 && s->videoOpaque() && rsx0 >= 0 &&
                           rsx1 <= layer.w && !tr.flipH;
        const size_t rowBytes = (size_t)(x1 - x0) * 4;

        for (int y = y0; y < y1; y++) {
            /* The mirrors are taken inside the kept region rather than inside
             * the whole decoded picture: what is being turned round is the
             * part that is on screen, so cropping the left of a shot and then
             * mirroring it puts that same part on the right. Doing it the
             * other way round would slide the picture as well as flip it. */
            const int sy = tr.flipV ? cy + (fh - 1 - (y - oy)) : cy + (y - oy);
            if (sy < 0 || sy >= layer.h) continue;

            uint8_t *d = m_canvas.data() + ((size_t)y * w + x0) * 4;
            const uint8_t *ss = layer.rgba.data() + (size_t)sy * layer.w * 4;

            if (solid) {
                std::memcpy(d, ss + (size_t)rsx0 * 4, rowBytes);
                continue;
            }

            for (int x = x0; x < x1; x++, d += 4) {
                const int sx = tr.flipH ? cx + (fw - 1 - (x - ox)) : cx + (x - ox);
                if (sx < 0 || sx >= layer.w) continue;
                const uint8_t *p = ss + (size_t)sx * 4;

                /* The source's own alpha, times the clip's fade.
                 *
                 * Ignoring the first of those is what made a transparent GIF
                 * laid over a video come out as a white box: the decoder puts
                 * the palette's colour in the transparent pixels and marks
                 * them with alpha 0, and a blit that copies three of the four
                 * channels is a blit that paints them anyway. */
                int a = p[3];
                if (fade < 255) a = a * fade / 255;
                if (a <= 0) continue;

                if (a >= 255) {
                    d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
                } else {
                    d[0] = (uint8_t)((p[0] * a + d[0] * (255 - a)) / 255);
                    d[1] = (uint8_t)((p[1] * a + d[1] * (255 - a)) / 255);
                    d[2] = (uint8_t)((p[2] * a + d[2] * (255 - a)) / 255);
                }
                d[3] = 255;
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
        /* A track pulled all the way down is skipped for the same reason a
         * clip at zero is: what comes out is silence either way, and the
         * decode that would produce it is the expensive part. */
        if (tr.kind != TRACK_AUDIO || tr.muted || tr.gain == 0.0) continue;

        for (const Clip &c : tr.clips) {
            if (c.muted || c.gain == 0.0) continue;

            /* The two levels multiply. See Track::gain. */
            const double gain = c.gain * tr.gain;

            const double a = std::max(bs, c.pos);
            const double b = std::min(be, c.end());
            if (b <= a + 1e-9) continue;

            const BinItem *bi = m_p->item(c.source);
            if (!bi || !bi->info.hasAudio) continue;
            Source *s = source(c.source, c.channel);
            if (!s || !s->hasAudio()) continue;

            int off = (int)std::lround((a - bs) * RATE);
            int n = (int)std::lround((b - a) * RATE);
            if (off < 0) off = 0;
            if (n <= 0) continue;
            if (off + n > frames) n = frames - off;
            if (n <= 0) continue;

            s->audioAt(c.srcAt(a), n, m_mix.data());

            /* The track's fx are evaluated per sample rather than per block:
             * a ramp shorter than a block would otherwise be a step, and a
             * step in a gain is a click. Only when there is one - which is
             * almost never - because that loop costs a multiply and a branch
             * per sample and the flat path is a straight scale. */
            bool ramp = false;
            for (size_t k = 1; k < tr.fx.size(); k++)
                if (tr.fx[k - 1].t < b && a < tr.fx[k].t) { ramp = true; break; }
            float *o = dst + (size_t)off * CHANS;

            if (!ramp) {
                const float g = (float)(gain * fxGain(tr, a));
                for (int i = 0; i < n * CHANS; i++) o[i] += m_mix[i] * g;
            } else {
                for (int i = 0; i < n; i++) {
                    const double ts = a + i / (double)RATE;
                    const float g = (float)(gain * fxGain(tr, ts));
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
