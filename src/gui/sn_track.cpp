/*
 * BENCsnip - the timeline
 *
 * The ruler, the track heads and the lanes. Every edit a mouse can make
 * happens in here; the keyboard's versions live in main.cpp and call the same
 * functions in sn_timeline.
 *
 * Two rules the whole pane is built on:
 *
 *   Nothing moves until the pointer does. Arming a drag on mouse-down and
 *   only acting once the pointer has travelled a few pixels is what lets a
 *   click select a clip without nudging it half a frame.
 *
 *   The playhead is where the sound is. Scrubbing seeks the player rather
 *   than moving a number the player later catches up with.
 */

#include "sn_app.h"

#include <algorithm>
#include <cmath>

namespace sn {

enum {
    RULER_H = 24,
    HEAD_W = 104,
    SCROLL_W = 11,          /* the bars along the right and the bottom */
    LANE_H = 56,
    LANE_GAP = 2,
    EDGE_GRAB = 7,       /* how close to an edge counts as the edge */
    FX_THIN = 11,        /* an empty effects lane, under its track   */
    FX_TALL = 30         /* ...and one with something on it          */
};

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

double App::timeAt(float x) const
{
    return scroll + (x - (rTimeline.x + HEAD_W)) / zoom;
}

float App::xAt(double t) const
{
    return (float)(rTimeline.x + HEAD_W + (t - scroll) * zoom);
}

/* --- how tall a track is ---
 *
 * Every track carries an effects lane under its clips: a thin strip when
 * there is nothing on it and a taller one when there is. So a track's block
 * is its clip lane plus that strip, and where the next block starts depends
 * on every block above it rather than on the index times a constant.
 *
 * It is walked rather than cached. There are a handful of tracks in a
 * project, this is a few additions per lookup, and a cached total is a thing
 * that goes stale the first time somebody adds an effect. */
static float fx_h(const Track &t)
{
    return t.fx.empty() ? (float)FX_THIN : (float)FX_TALL;
}

static float block_h(const Track &t) { return LANE_H + fx_h(t); }

static float block_top(const App &a, int idx)
{
    float y = a.rTimeline.y + RULER_H + 1 - a.trackScroll;
    const int n = (int)a.proj.tracks.size();
    for (int i = 0; i < idx && i < n; i++) y += block_h(a.proj.tracks[i]) + LANE_GAP;
    return y;
}

/* Where this track's clips are drawn. */
static Rectangle lane_rect(const App &a, int idx)
{
    return Rectangle{a.rTimeline.x + HEAD_W, block_top(a, idx),
                     a.rTimeline.width - HEAD_W - SCROLL_W, (float)LANE_H};
}

/* ...and the strip under it, where its effects are. */
static Rectangle fx_rect(const App &a, int idx)
{
    Rectangle l = lane_rect(a, idx);
    const float h = idx >= 0 && idx < (int)a.proj.tracks.size()
                        ? fx_h(a.proj.tracks[idx])
                        : (float)FX_THIN;
    return Rectangle{l.x, l.y + LANE_H, l.width, h};
}

/* How tall the track list wants to be, and how much room it has. */
static float tracks_height(const App &a)
{
    float h = 0;
    for (const Track &t : a.proj.tracks) h += block_h(t) + LANE_GAP;
    return h;
}

static float tracks_room(const App &a)
{
    return a.rTimeline.height - RULER_H - 1 - SCROLL_W;
}

/* The head covers the whole block, clips and effects lane together: they are
 * one track and one row of controls belongs to both. */
static Rectangle head_rect(const App &a, int idx)
{
    Rectangle l = lane_rect(a, idx);
    const float h = idx >= 0 && idx < (int)a.proj.tracks.size()
                        ? block_h(a.proj.tracks[idx])
                        : (float)LANE_H;
    return Rectangle{a.rTimeline.x, l.y, HEAD_W, h};
}

/* Which track block a y falls in, or -1. The effects strip counts as part of
 * its track, because it is. */
static int track_at(const App &a, float y)
{
    for (size_t i = 0; i < a.proj.tracks.size(); i++) {
        const float top = block_top(a, (int)i);
        const float bot = top + block_h(a.proj.tracks[i]);
        if (y >= top - LANE_GAP * 0.5f && y < bot + LANE_GAP * 0.5f) return (int)i;
    }
    return -1;
}

static Rectangle clip_rect(const App &a, int idx, const Clip &c)
{
    Rectangle l = lane_rect(a, idx);
    const float x0 = a.xAt(c.pos), x1 = a.xAt(c.end());
    return Rectangle{x0, l.y + 2, std::max(2.0f, x1 - x0), l.height - 4};
}

/* The same clip for the purpose of being clicked on, which is the whole clip
 * lane height rather than the drawn rectangle.
 *
 * A clip is drawn two pixels inside its lane, so a click in that two pixel
 * band was a click on no clip at all - and a click on no clip moves the
 * playhead. */
static Rectangle clip_hit_rect(const App &a, int idx, const Clip &c)
{
    Rectangle r = clip_rect(a, idx, c);
    Rectangle l = lane_rect(a, idx);
    return Rectangle{r.x, l.y, r.width, l.height};
}

static double snap_time(App &a, double t, const ClipRef *ignore)
{
    if (!a.snapping) return t;
    /* Ten pixels of tolerance, converted to seconds, so snapping feels the
     * same whatever the zoom. */
    return snap(a.proj, t, 10.0 / a.zoom, a.playhead, ignore);
}

/* ------------------------------------------------------------------ *
 * Drawing one clip
 * ------------------------------------------------------------------ */

static void draw_clip(App &a, int idx, const Clip &c, TrackKind kind, bool hot)
{
    sn_ui &ui = a.ui;
    Rectangle r = clip_rect(a, idx, c);
    Rectangle lane = lane_rect(a, idx);

    /* Off-screen either side: cheap to skip and there can be hundreds. */
    if (r.x > lane.x + lane.width || r.x + r.width < lane.x) return;

    const bool isSel = a.selected(ClipRef{idx, c.id});
    const bool video = kind == TRACK_VIDEO;
    const bool text = kind == TRACK_TEXT;

    const Color body = text  ? (isSel ? SN_CLIP_T_HI : SN_CLIP_T)
                      : video ? (isSel ? SN_CLIP_V_HI : SN_CLIP_V)
                              : (isSel ? SN_CLIP_A_HI : SN_CLIP_A);
    const Color edge = text ? SN_CLIP_T_EDGE : video ? SN_CLIP_V_EDGE : SN_CLIP_A_EDGE;

    BeginScissorMode((int)lane.x, (int)lane.y, (int)lane.width, (int)lane.height);

    sn_panel(r, body, isSel ? SN_TEXT : edge);
    if (c.muted) {
        /* Diagonal hatching: a muted clip has to be obvious without colour,
         * because colour is already saying video-or-audio. */
        for (float x = r.x - r.height; x < r.x + r.width; x += 8)
            DrawLineEx(Vector2{x, r.y + r.height}, Vector2{x + r.height, r.y}, 1,
                       Color{0, 0, 0, 90});
    }

    /* The source's first frame at the head of the clip - enough to tell two
     * clips apart at a glance without decoding anything here. */
    if (video && r.width > 30) {
        auto it = a.thumbs.find(c.source);
        if (it != a.thumbs.end() && it->second.ready && it->second.tex.id) {
            const Texture2D &tx = it->second.tex;
            const float th = r.height - 4;
            const float tw = th * ((float)tx.width / tx.height);
            if (tw < r.width - 4) {
                DrawTexturePro(tx, Rectangle{0, 0, (float)tx.width, (float)tx.height},
                               Rectangle{r.x + 2, r.y + 2, tw, th}, Vector2{0, 0}, 0,
                               Color{255, 255, 255, 90});
            }
        }
    }
    if (!video && !text && r.width > 12) {
        /* The real waveform, read off the file by the worker in sn_peaks.
         *
         * Every column asks what the source is playing at that moment, using
         * the same srcAt the renderer uses - so a trim shows the part that
         * was kept, and a looped clip repeats its shape at each wrap without
         * any of that being worked out here. */
        const BinItem *bi = a.proj.item(c.source);
        if (bi && !bi->missing) peaksAsk(c.source, bi->info.path);
        std::shared_ptr<const Peaks> pk = peaksGet(c.source);

        const float mid = r.y + r.height * 0.6f;
        const float x0 = std::max(r.x + 2, lane.x);
        const float x1 = std::min(r.x + r.width - 2, lane.x + lane.width);
        /* Dark on the clip rather than light. The clip is mid green and gets
         * brighter when it is selected, so a pale waveform reads on one and
         * washes out on the other - and the level line drawn over the top of
         * this is already pale. Two pale things on top of each other is one
         * thing nobody can read. */
        const Color wave = {0x1c, 0x2e, 0x14, 210};

        if (pk && pk->ready && !pk->hi.empty()) {
            const float amp = r.height * 0.34f;
            for (float x = x0; x < x1; x += 1.0f) {
                const double t = a.timeAt(x);
                if (t < c.pos || t >= c.end()) continue;

                /* The square root is presentation, not data: sound spends
                 * most of its time far below full scale, and a linear
                 * waveform of ordinary speech is a flat line with the
                 * occasional spike. */
                const float v = std::sqrt(pk->at(c.srcAt(t)));
                const float h = std::max(0.5f, amp * v);
                DrawLineEx(Vector2{x, mid - h}, Vector2{x, mid + h}, 1.0f, wave);
            }
        } else {
            /* Still reading it. A flat line rather than an invented shape:
             * the wrong waveform is worse than none, because somebody will
             * cut on it. */
            DrawLineEx(Vector2{x0, mid}, Vector2{x1, mid}, 1.0f,
                       Color{edge.r, edge.g, edge.b, 90});
        }
    }

    /* The level line: where the clip's gain sits between silence and +6 dB,
     * dragged with the mouse. Audio only - a video clip has no level, and a
     * line across it would be a control that does nothing. */
    if (kind == TRACK_AUDIO && r.width > 20) {
        const float ly = r.y + r.height * (1.0f - (float)(c.gain * 0.5)) ;
        DrawLineEx(Vector2{r.x + 1, ly}, Vector2{r.x + r.width - 1, ly}, 1.5f,
                   Color{0xcd, 0xea, 0xb0, (unsigned char)(isSel || hot ? 220 : 120)});
        if ((isSel || hot) && r.width > 70 && std::fabs(c.gain - 1.0) > 0.001) {
            char db[24];
            const double d = c.gain > 0.0001 ? 20.0 * std::log10(c.gain) : -60.0;
            snprintf(db, sizeof db, "%+.1f dB", d);

            /* On a dark patch of its own: the waveform is drawn underneath,
             * and light text on it is a number you have to squint at. */
            const float w = sn_measure(&ui, SN_F_TINY, db, 0.0f);
            const float x = r.x + (r.width - w) * 0.5f;
            DrawRectangleRec(Rectangle{x - 3, ly - 15, w + 6, 14}, Color{0, 0, 0, 150});
            sn_text(&ui, SN_F_TINY, db, x, ly - 14, SN_TEXT);
        }
    }

    /* Where the repeats fall. A looped clip that looks like an ordinary long
     * clip is a clip whose content nobody can predict; a line at each wrap
     * says "it starts again here" without a word. */
    if (c.looped() && r.width > 24) {
        const double cyc = c.cycle();
        if (cyc > 0.001) {
            for (double k = cyc; k < c.dur() - 0.001; k += cyc) {
                const float lx = a.xAt(c.pos + k);
                if (lx <= r.x || lx >= r.x + r.width) continue;
                for (float yy = r.y + 2; yy < r.y + r.height - 2; yy += 6)
                    DrawRectangle((int)lx, (int)yy, 1, 3, Color{0xcd, 0xea, 0xb0, 130});
            }
            if (r.width > 60)
                sn_draw_icon(SN_I_LOOP, Rectangle{r.x + r.width - 16, r.y + 3, 11, 11},
                             Color{0xcd, 0xea, 0xb0, 190});
        }
    }

    /* The name, and how long it is. Both go in a strip along the bottom
     * rather than over the picture at the head of the clip - text on top of a
     * thumbnail is readable in neither direction. */
    const BinItem *b = a.proj.item(c.source);

    /* A caption says what it says, across the body of the clip rather than in
     * the strip along the bottom: there is no thumbnail on a text clip for it
     * to be illegible over, and the words are the one thing about it worth
     * reading from across the timeline. Newlines become a middle dot, because
     * a clip is one row high and a caption is the shape of what it says. */
    if (text && r.width > 24) {
        std::string one;
        for (char ch : c.text.text) {
            /* U+00B7, written out as the two bytes it is. Appending the raw
             * 0xb7 instead is not UTF-8, and raylib draws a codepoint it
             * cannot decode as a question mark - which is what the first
             * version of this put in the middle of every two-line caption. */
            if (ch == '\n') one += "\xc2\xb7";
            else one += ch;
        }
        if (one.empty()) one = "(empty)";
        sn_text_clip(&ui, SN_F_SMALL, one.c_str(), r.x + 6, r.y + 6, r.width - 12,
                     c.text.text.empty() ? SN_EDGE : SN_TEXT);
    }

    if (r.width > 46) {
        Rectangle strip = {r.x + 1, r.y + r.height - 15, r.width - 2, 14};
        DrawRectangleRec(strip, Color{0, 0, 0, 90});

        static char label[256];
        const char *nm = text ? "caption" : (b ? b->info.name.c_str() : "?");
        if (c.channel >= 0) {
            /* Which channel, by the name anybody uses for it when there are
             * two, and by its number when there are more than two names to
             * remember. */
            const int nch = b ? b->info.chans : 0;
            const char *side = nch == 2 ? (c.channel == 0 ? "L" : "R") : nullptr;
            if (side) snprintf(label, sizeof label, "%s  %s", nm, side);
            else snprintf(label, sizeof label, "%s  ch%d", nm, c.channel + 1);
            nm = label;
        }
        sn_text_clip(&ui, SN_F_TINY, nm, strip.x + 4,
                     strip.y + 1, strip.width - (r.width > 110 ? 60 : 8), SN_TEXT);
        if (r.width > 110) {
            const std::string d = fmtTime(c.dur());
            sn_text(&ui, SN_F_TINY, d.c_str(),
                    strip.x + strip.width - sn_measure(&ui, SN_F_TINY, d.c_str(), 0) - 4,
                    strip.y + 1, Color{0xcd, 0xea, 0xb0, 170});
        }
    }

    /* Trim handles. */
    if (hot || isSel) {
        DrawRectangle((int)r.x, (int)r.y, 2, (int)r.height, SN_TEXT);
        DrawRectangle((int)(r.x + r.width) - 2, (int)r.y, 2, (int)r.height, SN_TEXT);
    }

    EndScissorMode();
}

/* Which track's effects strip the pointer is in, or -1. */
static int fx_lane_at(const App &a, Vector2 m, Rectangle r)
{
    if (m.x < r.x + HEAD_W) return -1;
    for (size_t i = 0; i < a.proj.tracks.size(); i++)
        if (CheckCollisionPointRec(m, fx_rect(a, (int)i))) return (int)i;
    return -1;
}

/* Where a level sits on screen inside the strip, and back again. Drawn inside
 * a margin so a point at 0 and a point at 1 are both fully visible rather
 * than half outside it. */
static float fx_y_of(const App &a, int idx, double v)
{
    Rectangle fr = fx_rect(a, idx);
    const float top = fr.y + 4, bot = fr.y + fr.height - 4;
    return bot - (float)v * (bot - top);
}

static double fx_v_of(const App &a, int idx, float y)
{
    Rectangle fr = fx_rect(a, idx);
    const float top = fr.y + 4, bot = fr.y + fr.height - 4;
    const double v = (bot - y) / std::max(1.0f, bot - top);
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* The point under the pointer on that lane, or -1. The nearest one within
 * reach rather than the first: points can be close together on a wave, and
 * the one being aimed at is the one closest to the pointer. */
static int fx_point_at(const App &a, int idx, Vector2 m)
{
    if (idx < 0 || idx >= (int)a.proj.tracks.size()) return -1;
    const Track &t = a.proj.tracks[idx];

    int best = -1;
    float bestd = 8.0f;
    for (size_t k = 0; k < t.fx.size(); k++) {
        const float x = a.xAt(t.fx[k].t), y = fx_y_of(a, idx, t.fx[k].v);
        const float d = std::max(std::fabs(m.x - x), std::fabs(m.y - y));
        if (d < bestd) { bestd = d; best = (int)k; }
    }
    return best;
}

/* ------------------------------------------------------------------ *
 * The effects lane
 *
 * The curve is drawn as the line it is, over a filled area beneath, so a
 * level reads as an amount and not only as a height, and the shape can be
 * read from across the timeline without hovering anything.
 *
 * The lane is thin and empty until something is on it. That is the whole of
 * "collapsed until used": there is no third state to draw and nothing to
 * expand, the strip is simply nineteen pixels taller once it has a ramp.
 * ------------------------------------------------------------------ */

static void draw_fx(App &a, int idx)
{
    if (idx < 0 || idx >= (int)a.proj.tracks.size()) return;
    const Track &t = a.proj.tracks[idx];
    Rectangle fr = fx_rect(a, idx);

    /* The stretch marked out with Shift, if it is this lane's. Drawn before
     * the curve so the curve stays readable over it. */
    if (a.fxSweepTrack == idx && a.fxSweepTo > a.fxSweepFrom) {
        const float x0 = a.xAt(a.fxSweepFrom), x1 = a.xAt(a.fxSweepTo);
        DrawRectangleRec(Rectangle{x0, fr.y, x1 - x0, fr.height},
                         Color{0x78, 0xb9, 0x46, 45});
        DrawLineEx(Vector2{x0, fr.y}, Vector2{x0, fr.y + fr.height}, 1.0f, SN_ACCENT);
        DrawLineEx(Vector2{x1, fr.y}, Vector2{x1, fr.y + fr.height}, 1.0f, SN_ACCENT);
    }

    if (t.fx.empty()) return;
    const float bot = fr.y + fr.height - 4;

    /* The line the curve makes, and the area under it, so a level reads as an
     * amount and not only as a height. A held point steps rather than slopes,
     * which is what makes a square wave square. */
    for (size_t k = 0; k + 1 < t.fx.size(); k++) {
        const FxPoint &p0 = t.fx[k], &p1 = t.fx[k + 1];
        const float x0 = a.xAt(p0.t), x1 = a.xAt(p1.t);
        if (x1 < fr.x || x0 > fr.x + fr.width) continue;

        const float y0 = fx_y_of(a, idx, p0.v);
        const float y1 = fx_y_of(a, idx, p0.hold ? p0.v : p1.v);

        sn_triangle(Vector2{x0, bot}, Vector2{x1, bot}, Vector2{x1, y1},
                    Color{0x78, 0xb9, 0x46, 55});
        sn_triangle(Vector2{x0, bot}, Vector2{x1, y1}, Vector2{x0, y0},
                    Color{0x78, 0xb9, 0x46, 55});

        DrawLineEx(Vector2{x0, y0}, Vector2{x1, y1}, 1.0f, SN_ACCENT);
        if (p0.hold)
            DrawLineEx(Vector2{x1, y1}, Vector2{x1, fx_y_of(a, idx, p1.v)}, 1.0f,
                       SN_ACCENT);
    }

    /* Before the first point and after the last the curve holds, and that is
     * drawn too - a fade at the end of a track should not look like it stops
     * being in force there. */
    {
        const float xf = a.xAt(t.fx.front().t), xl = a.xAt(t.fx.back().t);
        const float yf = fx_y_of(a, idx, t.fx.front().v);
        const float yl = fx_y_of(a, idx, t.fx.back().v);
        const Color faint = {0x78, 0xb9, 0x46, 90};
        if (xf > fr.x) DrawLineEx(Vector2{fr.x, yf}, Vector2{xf, yf}, 1.0f, faint);
        if (xl < fr.x + fr.width)
            DrawLineEx(Vector2{xl, yl}, Vector2{fr.x + fr.width, yl}, 1.0f, faint);
    }

    for (size_t k = 0; k < t.fx.size(); k++) {
        const float x = a.xAt(t.fx[k].t), y = fx_y_of(a, idx, t.fx[k].v);
        if (x < fr.x - 6 || x > fr.x + fr.width + 6) continue;

        const bool sel = a.fxSel.track == idx && a.fxSel.index == (int)k;
        const float g = sel ? 4.0f : 3.0f;
        DrawRectangleRec(Rectangle{x - g, y - g, g * 2, g * 2}, SN_BG);
        DrawRectangleLinesEx(Rectangle{x - g, y - g, g * 2, g * 2}, 1,
                             sel ? SN_TEXT : SN_ACCENT);
    }
}

/* ------------------------------------------------------------------ *
 * The ruler
 * ------------------------------------------------------------------ */

static void draw_ruler(App &a, Rectangle r)
{
    sn_ui &ui = a.ui;
    /* Only from the head column rightwards. The gutter to its left belongs to
     * the track buttons, and painting the whole strip here put them under it -
     * still clickable, which is the worst version of invisible. */
    Rectangle ruler = {r.x + HEAD_W, r.y, r.width - HEAD_W, RULER_H};
    DrawRectangleRec(ruler, SN_PANEL);

    /* A tick interval that gives a label every eighty pixels or so, from a
     * fixed set - otherwise the labels are at 0.37-second intervals and the
     * ruler is unreadable. */
    static const double steps[] = {0.04, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30,
                                   60, 120, 300, 600, 900, 1800, 3600};
    double step = 3600;
    for (double s : steps)
        if (s * a.zoom >= 70.0) { step = s; break; }

    const double t0 = a.timeAt(ruler.x);
    const double t1 = a.timeAt(ruler.x + ruler.width);

    BeginScissorMode((int)ruler.x, (int)ruler.y, (int)ruler.width, (int)ruler.height);

    /* Counted rather than accumulated: adding 0.2 to a double two hundred
     * times lands near enough to 40 to draw a tick and far enough from it to
     * label the tick 0:39. */
    const long k0 = (long)std::floor(t0 / step);
    const long k1 = (long)std::ceil(t1 / step) + 1;
    for (long k = k0; k <= k1; k++) {
        const double t = k * step;
        if (t < 0) continue;
        const float x = a.xAt(t);
        DrawLine((int)x, (int)(ruler.y + RULER_H - 7), (int)x, (int)(ruler.y + RULER_H),
                 SN_EDGE);

        /* Hundredths only when the ticks are closer together than a second -
         * a ruler labelled 00:05.00, 00:10.00 is four characters of noise per
         * label. */
        char lab[32];
        const int whole = (int)(t + 0.0005);
        if (step >= 1.0) {
            if (whole >= 3600) snprintf(lab, sizeof lab, "%d:%02d:%02d", whole / 3600,
                                        (whole / 60) % 60, whole % 60);
            else snprintf(lab, sizeof lab, "%d:%02d", whole / 60, whole % 60);
        } else {
            snprintf(lab, sizeof lab, "%d:%02d.%02d", whole / 60, whole % 60,
                     (int)((t - whole) * 100 + 0.5));
        }
        sn_text(&ui, SN_F_TINY, lab, x + 3, ruler.y + 4, SN_DIM);

        /* One unlabelled tick between labels, which is what makes the gap
         * readable as a duration rather than a gap. */
        const float xh = a.xAt(t + step * 0.5);
        DrawLine((int)xh, (int)(ruler.y + RULER_H - 4), (int)xh, (int)(ruler.y + RULER_H),
                 SN_EDGE);
    }
    EndScissorMode();

    sn_divider(r.x, r.y + RULER_H, r.width);
}

/* ------------------------------------------------------------------ *
 * Track heads
 * ------------------------------------------------------------------ */

static void draw_heads(App &a)
{
    sn_ui &ui = a.ui;

    /* The gutter above the heads, which is otherwise empty ruler. Two buttons
     * rather than one: what a person wants is either a video track or an
     * audio one, and a single + that then asks which is a dialog nobody
     * needs. */
    {
        Rectangle g = {a.rTimeline.x, a.rTimeline.y, HEAD_W, RULER_H};
        DrawRectangleRec(g, SN_PANEL);

        Rectangle bv = {g.x + 6, g.y + 3, 44, 18};
        Rectangle ba = {g.x + 54, g.y + 3, 44, 18};

        if (sn_button(&ui, 2900, bv, "+V", 1)) {
            const int at = addTrack(a.proj, TRACK_VIDEO);
            a.changed();
            a.say("added %s - the top row is the back of the picture",
                  a.proj.tracks[at].name.c_str());
        }
        if (sn_button(&ui, 2901, ba, "+A", 1)) {
            const int at = addTrack(a.proj, TRACK_AUDIO);
            a.changed();
            a.say("added %s", a.proj.tracks[at].name.c_str());
        }
        if (CheckCollisionPointRec(GetMousePosition(), bv) && !sn_ui_blocked(&ui))
            sn_tip(&ui, "add a video track. Video tracks all play at once - the top "
                        "row is the back, the bottom row is in front");
        if (CheckCollisionPointRec(GetMousePosition(), ba) && !sn_ui_blocked(&ui))
            sn_tip(&ui, "add an audio track. Audio tracks are mixed together");
    }

    BeginScissorMode((int)a.rTimeline.x, (int)(a.rTimeline.y + RULER_H + 1), HEAD_W,
                     (int)std::max(0.0f, tracks_room(a)));

    for (size_t i = 0; i < a.proj.tracks.size(); i++) {
        Track &t = a.proj.tracks[i];
        Rectangle h = head_rect(a, (int)i);
        if (h.y + h.height < a.rTimeline.y + RULER_H) continue;
        if (h.y > a.rTimeline.y + a.rTimeline.height) break;

        DrawRectangleRec(h, SN_PANEL);
        DrawLine((int)(h.x + h.width) - 1, (int)h.y, (int)(h.x + h.width) - 1,
                 (int)(h.y + h.height), SN_BORDER);

        /* The effects strip's own corner of the head, darker to match the
         * lane beside it, and labelled - a strip eleven pixels tall with
         * nothing in it needs to say what it is for. */
        {
            Rectangle fh = {h.x, h.y + LANE_H, h.width, h.height - LANE_H};
            DrawRectangleRec(fh, Color{0x0d, 0x12, 0x0b, 255});
            DrawLine((int)fh.x, (int)fh.y, (int)(fh.x + fh.width), (int)fh.y,
                     SN_BORDER);
            sn_text(&ui, SN_F_TINY, "FX", fh.x + 8, fh.y + (fh.height - 9) * 0.5f,
                    t.fx.empty() ? SN_EDGE : SN_ACCENT);
            if (!t.fx.empty()) {
                char n[24];
                snprintf(n, sizeof n, "%d", (int)t.fx.size());
                sn_text(&ui, SN_F_TINY, n, fh.x + 30, fh.y + (fh.height - 9) * 0.5f,
                        SN_DIM);
            }
        }

        sn_text_spaced(&ui, SN_F_SMALL, t.name.c_str(), h.x + 8, h.y + 5,
                       t.muted ? SN_EDGE : SN_TEXT);

        /* A track doing something to its picture says so, because four
         * numbers in a dialog are easy to forget having set. */
        if (t.kind == TRACK_VIDEO && t.transformed())
            sn_draw_icon(SN_I_CROP, Rectangle{h.x + 40, h.y + 6, 11, 11}, SN_ACCENT);

        const int id = 3000 + (int)i * 8;

        /* --- reorder, top right --- */
        /* Its band rather than its exact kind, so a caption can be moved in
         * front of a video track and behind another - which is the only way
         * to say which of them is on top. */
        const bool canUp = (int)i > 0 && sameBand(a.proj.tracks[i - 1].kind, t.kind);
        const bool canDn = i + 1 < a.proj.tracks.size() &&
                           sameBand(a.proj.tracks[i + 1].kind, t.kind);

        Rectangle up = {h.x + h.width - 38, h.y + 3, 16, 15};
        Rectangle dn = {h.x + h.width - 20, h.y + 3, 16, 15};

        if (sn_icon_button(&ui, id + 2, up, SN_I_UP, canUp, 0,
                           visualTrack(t.kind) ? "move up, which is further back"
                                                 : "move this track up") &&
            canUp) {
            moveTrack(a.proj, (int)i, -1);
            a.clearSel();
            a.changed();
        }
        if (sn_icon_button(&ui, id + 3, dn, SN_I_DOWN, canDn, 0,
                           visualTrack(t.kind) ? "move down, which is further forward"
                                                 : "move this track down") &&
            canDn) {
            moveTrack(a.proj, (int)i, 1);
            a.clearSel();
            a.changed();
        }

        /* --- the level, on audio tracks ---
         *
         * The strip between the name and the switches, which is the only room
         * a 104 pixel header has left, and only on audio tracks: there is
         * nothing on a video track to turn down.
         *
         * Same range and same snap as the level line across a clip - zero to
         * twice, unity in the middle, and anything within a hair of unity is
         * unity, because "back to where it was" is the adjustment made most
         * often and a mouse cannot land on 1.000 by hand. The two multiply,
         * which is what makes the fader worth having: one number that moves
         * everything on the track without disturbing what was set clip by
         * clip.
         */
        if (t.kind == TRACK_AUDIO) {
            Rectangle lv = {h.x + 6, h.y + 20, h.width - 12, 12};
            float v = (float)(t.gain * 0.5);

            if (sn_slider(&ui, id + 6, lv, &v)) {
                double g = v * 2.0;
                if (std::fabs(g - 1.0) < 0.04) g = 1.0;
                if (g != t.gain) {
                    t.gain = g;
                    a.gainTrack = (int)i;
                    a.changed(true);
                }
            }

            if (CheckCollisionPointRec(GetMousePosition(), lv) && !sn_ui_blocked(&ui)) {
                /* Decibels as well as the percentage, because a fader is
                 * read in decibels by everyone who has used one and the
                 * percentage is what the number underneath actually is. */
                if (t.gain <= 0.0)
                    sn_tip(&ui, "level: silent");
                else
                    sn_tip(&ui, "level: %+.1f dB (%.0f%%)%s", 20.0 * std::log10(t.gain),
                           t.gain * 100.0, t.muted ? " - and the track is muted" : "");
            }
        }

        /* --- the switches, along the bottom of the clip half ---
         *
         * h.height now covers the effects strip as well, so this is measured
         * from the clip lane rather than from the bottom of the head. */
        const float by = h.y + LANE_H - 23;
        Rectangle b1 = {h.x + 6, by, 20, 18};
        Rectangle b2 = {h.x + 29, by, 20, 18};
        Rectangle b3 = {h.x + 52, by, 20, 18};
        Rectangle b4 = {h.x + 78, by, 20, 18};

        if (visualTrack(t.kind)) {
            if (sn_icon_button(&ui, id, b1, t.muted ? SN_I_EYE_OFF : SN_I_EYE, 1, t.muted,
                               t.muted ? "show this track" : "hide this track")) {
                t.muted = !t.muted;
                a.changed(true);
            }
        } else {
            if (sn_icon_button(&ui, id, b1, t.muted ? SN_I_MUTE : SN_I_SPEAKER, 1, t.muted,
                               t.muted ? "unmute this track" : "mute this track")) {
                t.muted = !t.muted;
                a.changed(true);
            }
        }
        if (sn_icon_button(&ui, id + 1, b2, t.locked ? SN_I_LOCK : SN_I_UNLOCK, 1, t.locked,
                           t.locked ? "unlock this track" : "lock this track")) {
            t.locked = !t.locked;
            a.changed(true);
        }

        if (t.kind == TRACK_VIDEO) {
            if (sn_icon_button(&ui, id + 4, b3, SN_I_CROP, 1, t.transformed(),
                               "size, position and crop for this track")) {
                a.layoutTrack = (int)i;
                a.modal = MODAL_LAYOUT;
            }
        }

        /* Only ever one of a kind left: something has to be there to drop a
         * file onto, and a delete that silently does nothing is worse than a
         * button that says it cannot.
         *
         * Text tracks have no such floor - nothing is ever dropped on one, and
         * a project with no captions is an ordinary project - so the last one
         * goes when it is asked to. */
        int sameKind = 0;
        for (const Track &x : a.proj.tracks)
            if (x.kind == t.kind) sameKind++;
        const bool canDrop = t.kind == TRACK_TEXT || sameKind > 1;

        if (sn_icon_button(&ui, id + 5, b4, SN_I_TRASH, canDrop, 0,
                           canDrop ? "delete this track and everything on it"
                                   : "the last track of its kind stays") &&
            canDrop) {
            const std::string nm = t.name;
            removeTrack(a.proj, (int)i);
            a.clearSel();
            a.changed();
            a.say("deleted %s", nm.c_str());
            break;   /* the vector moved under us */
        }
    }

    /* One undo entry for the whole adjustment. While the fader moves the
     * change is minor, the way a clip being dragged is; the commit happens
     * when the button comes up, and here rather than beside the slider
     * because sn_ui_frame has already cleared `active` by then. */
    if (a.gainTrack >= 0 && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        a.gainTrack = -1;
        a.changed();
    }

    EndScissorMode();
}

/* ------------------------------------------------------------------ *
 * The pane
 * ------------------------------------------------------------------ */

void timelinePane(App &a, Rectangle r)
{
    sn_ui &ui = a.ui;
    a.rTimeline = r;

    const Vector2 m = GetMousePosition();
    const bool inside = CheckCollisionPointRec(m, r) && !sn_ui_blocked(&ui);

    DrawRectangleRec(r, SN_WELL);

    /* --- wheel: scroll, or zoom with a modifier --- */
    if (inside) {
        const float w = GetMouseWheelMove();
        if (w != 0) {
            if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
                zoomTo(a, a.timeAt(m.x), a.zoom * (w > 0 ? 1.25 : 0.8));
            } else if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                zoomTo(a, a.timeAt(m.x), a.zoom * (w > 0 ? 1.25 : 0.8));
            } else if (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT) ||
                       m.x < r.x + HEAD_W) {
                /* Over the heads, or with Alt held: the wheel moves the track
                 * list rather than time. */
                a.trackScroll -= w * 40.0f;
            } else {
                a.scroll -= w * 60.0 / a.zoom;
                if (a.scroll < 0) a.scroll = 0;
                a.follow = false;
            }
        }
        /* Middle-drag pans, which is the one gesture every timeline in the
         * world shares. */
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            a.scroll -= GetMouseDelta().x / a.zoom;
            if (a.scroll < 0) a.scroll = 0;
            a.follow = false;
        }
    }

    /* --- lanes ---
     *
     * Clipped, because with a scrolled list the first track can be halfway
     * under the ruler and the last one halfway off the bottom. */
    BeginScissorMode((int)r.x, (int)(r.y + RULER_H + 1), (int)r.width,
                     (int)std::max(0.0f, tracks_room(a)));
    for (size_t i = 0; i < a.proj.tracks.size(); i++) {
        Rectangle l = lane_rect(a, (int)i);
        if (l.y > r.y + r.height) break;
        DrawRectangleRec(l, visualTrack(a.proj.tracks[i].kind)
                                ? Color{0x0e, 0x14, 0x0c, 255}
                                : Color{0x0c, 0x12, 0x0a, 255});
        if (a.proj.tracks[i].locked)
            for (float x = l.x; x < l.x + l.width; x += 10)
                DrawLineEx(Vector2{x, l.y}, Vector2{x + l.height, l.y + l.height}, 1,
                           Color{0x2a, 0x3a, 0x1e, 60});

        /* The effects lane, darker than the clips above it so the eye reads
         * two rows rather than one tall one. */
        Rectangle fr = fx_rect(a, (int)i);
        DrawRectangleRec(fr, Color{0x08, 0x0c, 0x07, 255});
        DrawLine((int)fr.x, (int)fr.y, (int)(fr.x + fr.width), (int)fr.y,
                 Color{0x1a, 0x24, 0x14, 255});
    }

    EndScissorMode();

    /* Second-lines down the lanes, so a clip's length is readable against
     * something. */
    {
        const float bottom = lane_rect(a, (int)a.proj.tracks.size()).y;
        double step = 1.0;
        while (step * a.zoom < 12) step *= 5;
        BeginScissorMode((int)(r.x + HEAD_W), (int)(r.y + RULER_H), (int)(r.width - HEAD_W),
                         (int)(r.height - RULER_H));
        for (double t = std::floor(a.timeAt(r.x + HEAD_W) / step) * step;
             t < a.timeAt(r.x + r.width) + step; t += step) {
            if (t < 0) continue;
            DrawLine((int)a.xAt(t), (int)(r.y + RULER_H + 1), (int)a.xAt(t), (int)bottom,
                     Color{0x2a, 0x3a, 0x1e, 60});
        }
        EndScissorMode();
    }

    /* --- what the pointer is over --- */
    int overTrack = inside && m.x > r.x + HEAD_W ? track_at(a, m.y) : -1;
    ClipRef overClip{-1, -1};
    DragKind overWhat = DRAG_NONE;

    if (overTrack >= 0) {
        const Track &t = a.proj.tracks[overTrack];
        for (const Clip &c : t.clips) {
            if (!CheckCollisionPointRec(m, clip_hit_rect(a, overTrack, c))) continue;
            Rectangle cr = clip_rect(a, overTrack, c);

            overClip = ClipRef{overTrack, c.id};

            const bool wide = cr.width > 24;

            if (wide && m.x < cr.x + EDGE_GRAB) overWhat = DRAG_TRIM_IN;
            else if (wide && m.x > cr.x + cr.width - EDGE_GRAB) overWhat = DRAG_TRIM_OUT;
            else if (t.kind == TRACK_AUDIO &&
                     std::fabs(m.y - (cr.y + cr.height * (1.0f - (float)(c.gain * 0.5)))) < 5)
                overWhat = DRAG_GAIN;
            else overWhat = DRAG_CLIP;
            break;
        }
    }

    /* --- what the pointer will do here, said with its shape ---
     *
     * The shape is the only part of a drag that is discoverable before you
     * commit to it: the edge of a clip looks exactly like the middle of one
     * until the cursor changes. */
    if (inside) {
        switch (overWhat) {
        case DRAG_TRIM_OUT: {
            /* Past the end of the source, dragging this edge repeats the clip
             * rather than lengthening it, and there is no system cursor that
             * says so - so one gets drawn. */
            const Clip *c = a.proj.clip(overClip);
            const BinItem *b = c ? a.proj.item(c->source) : nullptr;
            const bool beyond = c && b && b->info.duration > 0 &&
                                (c->looped() ||
                                 a.timeAt(m.x) > c->pos + (b->info.duration - c->in) + 1e-6);
            if (beyond) sn_cursor_glyph(&ui, SN_I_LOOP);
            else sn_cursor(&ui, MOUSE_CURSOR_RESIZE_EW);
            break;
        }
        case DRAG_TRIM_IN:
        case DRAG_FX_SWEEP: sn_cursor(&ui, MOUSE_CURSOR_RESIZE_EW); break;
        case DRAG_FX: sn_cursor(&ui, MOUSE_CURSOR_RESIZE_ALL); break;
        case DRAG_GAIN:     sn_cursor(&ui, MOUSE_CURSOR_RESIZE_NS); break;
        case DRAG_CLIP:     sn_cursor(&ui, MOUSE_CURSOR_RESIZE_ALL); break;
        default:
            /* The ruler scrubs, and so does empty timeline. */
            if (m.x > r.x + HEAD_W) sn_cursor(&ui, MOUSE_CURSOR_RESIZE_EW);
            break;
        }
    }

    /* A drag already in progress keeps its shape wherever the pointer has
     * wandered to, which is what tells you it is still holding on. */
    switch (a.drag) {
    case DRAG_TRIM_OUT: {
        const Clip *c = a.proj.clip(a.dragClip);
        if (c && c->looped()) sn_cursor_glyph(&ui, SN_I_LOOP);
        else sn_cursor(&ui, MOUSE_CURSOR_RESIZE_EW);
        break;
    }
    case DRAG_TRIM_IN:
    case DRAG_FX_SWEEP:
    case DRAG_SCRUB:    sn_cursor(&ui, MOUSE_CURSOR_RESIZE_EW); break;
    case DRAG_GAIN:     sn_cursor(&ui, MOUSE_CURSOR_RESIZE_NS); break;
    case DRAG_CLIP:
    case DRAG_FROM_BIN: sn_cursor(&ui, MOUSE_CURSOR_RESIZE_ALL); break;
    default: break;
    }

    /* --- clips --- */
    BeginScissorMode((int)r.x, (int)(r.y + RULER_H + 1), (int)r.width,
                     (int)std::max(0.0f, tracks_room(a)));
    for (size_t i = 0; i < a.proj.tracks.size(); i++) {
        const Track &t = a.proj.tracks[i];
        if (lane_rect(a, (int)i).y > r.y + r.height) break;
        for (const Clip &c : t.clips)
            draw_clip(a, (int)i, c, t.kind,
                      overClip.track == (int)i && overClip.clip == c.id);
        draw_fx(a, (int)i);
    }
    EndScissorMode();

    draw_ruler(a, r);
    draw_heads(a);

    /* --- starting a drag --- */
    const int fxLane = fx_lane_at(a, m, r);

    if (inside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && a.drag == DRAG_NONE) {
        if (m.y < r.y + RULER_H && m.x > r.x + HEAD_W) {
            a.drag = DRAG_SCRUB;
            a.follow = false;
        } else if (fxLane >= 0 && !a.proj.tracks[fxLane].locked) {
            /* The effects lane. Tested before the clips and before the click
             * that selects nothing, because it is inside a track's block and
             * would otherwise be neither.
             *
             * One rule: a press on a point picks that point up, and a press
             * anywhere else puts a new one there and picks that up. So the
             * first point on an empty lane and the ninth on a busy one are
             * the same gesture, and there is no mode to be in. */
            Track &t = a.proj.tracks[fxLane];
            const bool sweeping = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

            if (sweeping) {
                /* Shift marks out a stretch for a preset to be applied over,
                 * rather than adding a point. The other ways of saying where
                 * a preset goes are all somebody else's idea of a range - the
                 * selection, the clip under the pointer, the whole track -
                 * and sometimes the one wanted is none of them. */
                a.fxSweepTrack = fxLane;
                a.fxSweepFrom = a.fxSweepTo = std::max(0.0, a.timeAt(m.x));
                a.fxSel = App::FxRef{};
                a.dragFrom = m;
                a.dragMoved = false;
                a.drag = DRAG_FX_SWEEP;
            } else {
                int k = fx_point_at(a, fxLane, m);

                /* A plain press puts a point down, and drops any swept range:
                 * the band would otherwise sit there long after it meant
                 * anything. */
                a.fxSweepTrack = -1;

                if (k < 0) {
                    FxPoint np;
                    np.t = std::max(0.0, snap_time(a, a.timeAt(m.x), nullptr));
                    np.v = fx_v_of(a, fxLane, m.y);
                    t.fx.push_back(np);
                    fxTidy(t);

                    k = -1;
                    for (size_t i = 0; i < t.fx.size(); i++)
                        if (std::fabs(t.fx[i].t - np.t) < 1e-9) k = (int)i;
                }

                a.fxSel = App::FxRef{fxLane, k};
                a.fxDrag = a.fxSel;
                a.dragFrom = m;
                a.dragMoved = false;
                a.drag = DRAG_FX;
            }
        } else if (overClip.ok() && !a.proj.tracks[overClip.track].locked) {
            const Clip *c = a.proj.clip(overClip);
            a.select(overClip, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
            a.drag = overWhat;
            a.dragClip = overClip;
            a.dragFrom = m;
            a.dragMoved = false;
            a.dragGrab = a.timeAt(m.x) - (c ? c->pos : 0);
        } else if (overTrack >= 0) {
            /* Clicking empty timeline selects nothing, and that is all.
             *
             * It used to move the playhead as well, on the grounds that it
             * saves a trip to the ruler. What it actually saves is the trip;
             * what it costs is every click that was meant for something else -
             * a clip on a locked track, the two pixels above a clip, an empty
             * stretch beside one - throwing the playhead across the timeline
             * and the sound with it. The ruler is where scrubbing lives. */
            a.clearSel();
        }
    }

    /* --- carrying it on --- */
    if (a.drag != DRAG_NONE && (IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
                                a.drag == DRAG_FROM_BIN)) {
        if (std::fabs(m.x - a.dragFrom.x) > 3 || std::fabs(m.y - a.dragFrom.y) > 3)
            a.dragMoved = true;

        switch (a.drag) {
        case DRAG_SCRUB: {
            double t = std::max(0.0, a.timeAt(m.x));
            t = snap_time(a, t, nullptr);
            if (t != a.playhead) {
                a.playhead = t;
                a.player.seek(t);
            }
            break;
        }
        case DRAG_CLIP: {
            if (!a.dragMoved) break;
            double t = snap_time(a, a.timeAt(m.x) - a.dragGrab, &a.dragClip);
            if (t < 0) t = 0;
            int tr = track_at(a, m.y);
            if (tr < 0) tr = a.dragClip.track;
            moveClip(a.proj, a.dragClip, tr, t);
            a.sel.clear();
            a.sel.push_back(a.dragClip);
            a.changed(true);
            break;
        }
        case DRAG_TRIM_IN:
        case DRAG_TRIM_OUT: {
            if (!a.dragMoved) break;
            double t = snap_time(a, a.timeAt(m.x), &a.dragClip);
            trimClip(a.proj, a.dragClip, a.drag == DRAG_TRIM_IN, t);
            a.changed(true);
            break;
        }
        case DRAG_FX_SWEEP: {
            const double now = std::max(0.0, snap_time(a, a.timeAt(m.x), nullptr));
            const double at0 = std::max(0.0, a.timeAt(a.dragFrom.x));
            a.fxSweepFrom = std::min(at0, now);
            a.fxSweepTo = std::max(at0, now);
            break;
        }
        case DRAG_FX: {
            Track *t = a.proj.track(a.fxDrag.track);
            if (!t || a.fxDrag.index < 0 || a.fxDrag.index >= (int)t->fx.size()) break;

            FxPoint &pt = t->fx[(size_t)a.fxDrag.index];
            pt.t = std::max(0.0, snap_time(a, a.timeAt(m.x), nullptr));
            pt.v = fx_v_of(a, a.fxDrag.track, m.y);
            a.changed(true);
            break;
        }
        case DRAG_GAIN: {
            Clip *c = a.proj.clip(a.dragClip);
            if (!c) break;
            Rectangle cr = clip_rect(a, a.dragClip.track, *c);
            double g = (1.0 - (m.y - cr.y) / (double)cr.height) * 2.0;
            /* Snapping to unity, because "back to where it was" is the
             * adjustment people make most often and a mouse cannot land on
             * 1.000 by hand. */
            if (std::fabs(g - 1.0) < 0.04) g = 1.0;
            c->gain = std::max(0.0, std::min(2.0, g));
            a.changed(true);
            break;
        }
        default:
            break;
        }
    }

    /* --- a bin item being dragged in --- */
    if (a.drag == DRAG_FROM_BIN) {
        const BinItem *b = a.proj.item(a.dragBin);
        if (b && inside && overTrack >= 0) {
            double t = std::max(0.0, snap_time(a, a.timeAt(m.x), nullptr));
            const float x0 = a.xAt(t);
            const float w = (float)(b->info.duration * a.zoom);
            Rectangle lane = lane_rect(a, overTrack);
            DrawRectangleRec(Rectangle{x0, lane.y + 2, std::max(3.0f, w), lane.height - 4},
                             Color{0x78, 0xb9, 0x46, 70});
            DrawRectangleLinesEx(Rectangle{x0, lane.y + 2, std::max(3.0f, w), lane.height - 4},
                                 1, SN_ACCENT);
            sn_tip(&ui, "drop to put %s here at %s", b->info.name.c_str(),
                   fmtTime(t).c_str());
        }

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (b && inside && overTrack >= 0) {
                double t = std::max(0.0, snap_time(a, a.timeAt(m.x), nullptr));
                /* Dropping onto an audio track puts only the audio there;
                 * onto a video track, the pair. Which is what the pointer was
                 * pointing at. */
                /* Dropping onto an audio track puts only the audio there.
                 * Onto a text track, neither half belongs where it landed, so
                 * both go wherever they would have gone anyway rather than
                 * turning a caption track into a video one. */
                const TrackKind lane = a.proj.tracks[overTrack].kind;
                const bool audioLane = lane == TRACK_AUDIO;
                const bool textLane = lane == TRACK_TEXT;
                placeItem(a.proj, a.dragBin, t,
                          audioLane ? NO_TRACK : textLane ? -1 : overTrack,
                          audioLane ? overTrack : -1);
                a.changed();
                a.say("added %s at %s", b->info.name.c_str(), fmtTime(t).c_str());
            }
            a.drag = DRAG_NONE;
            a.dragBin = 0;
        }
    }

    /* --- finishing --- */
    if (a.drag != DRAG_NONE && a.drag != DRAG_FROM_BIN &&
        IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (a.drag == DRAG_FX) {
            Track *t = a.proj.track(a.fxDrag.track);
            if (t) {
                /* Remember which point this was by where it is, because
                 * tidying sorts the vector and the index it had is not the
                 * index it keeps. */
                double at = -1;
                if (a.fxDrag.index >= 0 && a.fxDrag.index < (int)t->fx.size())
                    at = t->fx[(size_t)a.fxDrag.index].t;
                fxTidy(*t);

                a.fxSel = App::FxRef{};
                for (size_t k = 0; k < t->fx.size(); k++)
                    if (std::fabs(t->fx[k].t - at) < 1e-9)
                        a.fxSel = App::FxRef{a.fxDrag.track, (int)k};
            }
            a.fxDrag = App::FxRef{};
            a.changed();
        } else if (a.dragMoved && a.drag != DRAG_SCRUB) {
            a.changed();
        }
        a.drag = DRAG_NONE;
    }

    /* --- right-click the effects lane --- */
    if (inside && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && fxLane >= 0) {
        /* --- where a preset would go ---
         *
         * Four answers, most specific first, because each is a narrower claim
         * about what somebody meant than the one after it:
         *
         *   a stretch marked out with Shift    they said exactly
         *   the clips selected on this track   they pointed at it
         *   the clip under the pointer         they right-clicked on it
         *   everything on the track            there is nothing else to mean
         *
         * The rows say which one it landed on. A menu that silently applies
         * to a range you did not have in mind is worse than one that makes
         * you read four extra words.
         */
        const Track &ft = a.proj.tracks[fxLane];
        const char *what = nullptr;
        a.fxRangeFrom = a.fxRangeTo = 0.0;

        if (a.fxSweepTrack == fxLane && a.fxSweepTo > a.fxSweepFrom) {
            a.fxRangeFrom = a.fxSweepFrom;
            a.fxRangeTo = a.fxSweepTo;
            what = "the marked stretch";
        }

        if (!what) {
            double lo = 0, hi = 0;
            bool any = false;
            for (const ClipRef &sr : a.sel) {
                if (sr.track != fxLane) continue;
                const Clip *sc = a.proj.clip(sr);
                if (!sc) continue;
                if (!any) { lo = sc->pos; hi = sc->end(); any = true; }
                else { lo = std::min(lo, sc->pos); hi = std::max(hi, sc->end()); }
            }
            if (any && hi > lo) {
                a.fxRangeFrom = lo;
                a.fxRangeTo = hi;
                what = a.sel.size() > 1 ? "the selected clips" : "the selected clip";
            }
        }

        if (!what) {
            const Clip *c = ft.at(a.timeAt(m.x));
            if (c) {
                a.fxRangeFrom = c->pos;
                a.fxRangeTo = c->end();
                what = "this clip";
            }
        }

        if (!what && !ft.clips.empty()) {
            a.fxRangeFrom = ft.clips.front().pos;
            a.fxRangeTo = ft.clips.back().end();
            what = "the whole track";
        }

        const int k = fx_point_at(a, fxLane, m);
        a.fxSel = App::FxRef{fxLane, k};

        static const char *fitems[10];
        static char rows[5][64];
        int n = 0;
        a.fxMenu.clear();
        auto add = [&](const char *label, App::FxAction act) {
            fitems[n++] = label;
            a.fxMenu.push_back((int)act);
        };

        if (what) {
            static const char *shape[5] = {"fade in over", "fade out over",
                                           "in and out over", "pulse over",
                                           "wave over"};
            static const App::FxAction acts[5] = {App::FX_M_IN, App::FX_M_OUT,
                                                  App::FX_M_INOUT, App::FX_M_PULSE,
                                                  App::FX_M_WAVE};
            for (int i = 0; i < 5; i++) {
                snprintf(rows[i], sizeof rows[i], "%s %s", shape[i], what);
                add(rows[i], acts[i]);
            }
        }
        if (k >= 0) {
            if (n) add("-", App::FX_M_NOTHING);
            add(a.proj.tracks[fxLane].fx[(size_t)k].hold ? "let this point slide"
                                                         : "hold this point",
                App::FX_M_HOLD);
            add("delete this point", App::FX_M_DELETE);
        }
        if (!a.proj.tracks[fxLane].fx.empty()) {
            if (n) add("-", App::FX_M_NOTHING);
            add("clear the lane", App::FX_M_CLEAR);
        }

        if (n) sn_menu_open(&ui, m, fitems, n, 102);
    }

    /* --- double-click a caption: the window with the words in it ---
     *
     * The same gesture the preview offers on the caption itself, because the
     * timeline is where a clip is found when the playhead is somewhere else
     * and the caption is not on screen to be double-clicked. */
    if (inside && overClip.ok() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        a.proj.tracks[overClip.track].kind == TRACK_TEXT &&
        sn_double_click(&ui, 5400 + overClip.clip)) {
        a.select(overClip, false);
        a.textClip = overClip;
        a.modal = MODAL_TEXT;
    }

    /* --- right-click --- */
    if (inside && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && overClip.ok() &&
        a.proj.tracks[overClip.track].kind == TRACK_TEXT) {
        /* Its own list on its own tag rather than items bolted onto the one
         * below. Half of that list is about sound or about a linked video
         * half, a caption has neither, and the handler there finds its last
         * item by counting - which is exactly the arrangement that breaks
         * when somebody adds a seventh item to one case out of two. */
        static const char *titems[] = {"edit the caption...", "-", "split here",
                                       "delete", "delete and close the gap"};
        a.select(overClip, false);
        a.textClip = overClip;
        sn_menu_open(&ui, m, titems, 5, 101);
    } else if (inside && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && overClip.ok()) {
        const Clip *rc = a.proj.clip(overClip);
        const bool linked = rc && rc->link != 0;

        static const char *items[8];
        static char linkItem[64];
        static char chanItem[64];
        int n = 0;

        a.clipMenu.clear();
        auto add = [&](const char *label, App::ClipAction what) {
            items[n++] = label;
            a.clipMenu.push_back((int)what);
        };

        add("split here", App::CLIP_SPLIT);
        add("delete", App::CLIP_DELETE);
        add("delete and close the gap", App::CLIP_RIPPLE);
        add("-", App::CLIP_NOTHING);
        add("mute", App::CLIP_MUTE);
        add("clear this track's effects", App::CLIP_CLEAR_FX);

        /* Only where there is something to split: a mono file is already one
         * channel, and a clip that has been split has one of its own. */
        const BinItem *rb = rc ? a.proj.item(rc->source) : nullptr;
        if (rc && rc->channel < 0 && rb && rb->info.hasAudio && rb->info.chans > 1 &&
            a.proj.tracks[overClip.track].kind == TRACK_AUDIO) {
            snprintf(chanItem, sizeof chanItem, "split its %d channels apart",
                     rb->info.chans);
            add(chanItem, App::CLIP_SPLIT_CHANNELS);
        }

        if (linked) {
            snprintf(linkItem, sizeof linkItem, "unlink from its %s",
                     a.proj.tracks[overClip.track].kind == TRACK_VIDEO ? "audio"
                                                                       : "video");
            add(linkItem, App::CLIP_UNLINK);
        }

        a.select(overClip, false);
        sn_menu_open(&ui, m, items, n, 100);
    }

    /* --- the bars ---
     *
     * A wheel and a middle-drag were the only way to know where you were in a
     * long timeline, and neither of them shows you. A bar is the one control
     * that answers "how much is there and where am I in it" without being
     * touched.
     * ------------------------------------------------------------------ */
    {
        const float lanesX = r.x + HEAD_W;
        const float lanesW = r.width - HEAD_W - SCROLL_W;

        /* --- along the bottom: time --- */
        const double visible = lanesW / a.zoom;
        const double content = std::max(a.proj.duration() + visible * 0.25, visible);

        Rectangle track = {lanesX, r.y + r.height - SCROLL_W, lanesW, SCROLL_W - 2};
        DrawRectangleRec(track, SN_WELL);

        const float frac = (float)std::min(1.0, visible / content);
        const float thumbW = std::max(28.0f, track.width * frac);
        const float span = track.width - thumbW;
        const float at = content > visible
                             ? (float)(a.scroll / (content - visible)) : 0.0f;

        Rectangle thumb = {track.x + span * std::min(1.0f, std::max(0.0f, at)),
                           track.y + 1, thumbW, track.height - 2};

        const bool hotH = CheckCollisionPointRec(m, track) && !sn_ui_blocked(&ui);
        if (hotH || ui.active == 4001) sn_cursor(&ui, MOUSE_CURSOR_RESIZE_EW);

        if (hotH && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            ui.active = 4001;
            /* Clicking the trough jumps there; clicking the thumb grabs it
             * where it was held, so it does not leap under the pointer. */
            a.dragGrab = CheckCollisionPointRec(m, thumb) ? (m.x - thumb.x) : thumbW * 0.5;
            a.follow = false;
        }
        if (ui.active == 4001 && span > 0) {
            const float want = (m.x - (float)a.dragGrab - track.x) / span;
            a.scroll = std::max(0.0, std::min(1.0f, std::max(0.0f, want)) *
                                         (content - visible));
        }

        DrawRectangleRounded(thumb, 0.4f, 4,
                             (hotH || ui.active == 4001) ? SN_DIM : SN_EDGE);

        /* --- down the right: tracks, but only when there are too many --- */
        const float want = tracks_height(a), room = tracks_room(a);
        Rectangle vt = {r.x + r.width - SCROLL_W, r.y + RULER_H + 1, SCROLL_W - 2, room};

        if (want > room) {
            DrawRectangleRec(vt, SN_WELL);

            const float th = std::max(24.0f, vt.height * (room / want));
            const float vspan = vt.height - th;
            const float vat = a.trackScroll / (want - room);

            Rectangle vthumb = {vt.x + 1, vt.y + vspan * std::min(1.0f, std::max(0.0f, vat)),
                                vt.width - 2, th};

            const bool hotV = CheckCollisionPointRec(m, vt) && !sn_ui_blocked(&ui);
            if (hotV || ui.active == 4002) sn_cursor(&ui, MOUSE_CURSOR_RESIZE_NS);

            if (hotV && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                ui.active = 4002;
                a.dragGrab = CheckCollisionPointRec(m, vthumb) ? (m.y - vthumb.y) : th * 0.5;
            }
            if (ui.active == 4002 && vspan > 0) {
                const float w2 = (m.y - (float)a.dragGrab - vt.y) / vspan;
                a.trackScroll = std::min(1.0f, std::max(0.0f, w2)) * (want - room);
            }

            DrawRectangleRounded(vthumb, 0.4f, 4,
                                 (hotV || ui.active == 4002) ? SN_DIM : SN_EDGE);
        } else {
            a.trackScroll = 0;
        }

        a.trackScroll = std::max(0.0f, std::min(a.trackScroll, std::max(0.0f, want - room)));
    }

    /* --- the playhead, over everything --- */
    {
        const float x = a.xAt(a.playhead);
        if (x >= r.x + HEAD_W - 1 && x <= r.x + r.width) {
            const float bottom =
                std::min(r.y + r.height, lane_rect(a, (int)a.proj.tracks.size()).y);
            DrawLine((int)x, (int)r.y, (int)x, (int)bottom, SN_AMBER);
            DrawTriangle(Vector2{x - 6, r.y}, Vector2{x, r.y + 8}, Vector2{x + 6, r.y},
                         SN_AMBER);
        }
    }

    /* --- what the pointer is over, for the status line --- */
    if (inside && overClip.ok()) {
        const Clip *c = a.proj.clip(overClip);
        const BinItem *b = c ? a.proj.item(c->source) : nullptr;
        if (c && b) {
            const char *verb = overWhat == DRAG_TRIM_IN    ? "drag to trim the start"
                               : overWhat == DRAG_TRIM_OUT
                                   ? (c->looped()
                                          ? "drag to change how many times it repeats"
                                          : "drag to trim the end, or past it to loop")
                               : overWhat == DRAG_FX ? "drag the point about"
                               : overWhat == DRAG_GAIN     ? "drag the level up or down"
                                                           : "drag to move it";
            sn_tip(&ui, "%s  %s of %s  -  %s", b->info.name.c_str(),
                   fmtTime(c->dur()).c_str(), fmtTime(b->info.duration).c_str(), verb);
        }
    } else if (inside && fxLane >= 0 && a.drag == DRAG_NONE) {
        const Track &ht = a.proj.tracks[fxLane];
        if (fx_point_at(a, fxLane, m) >= 0) {
            sn_cursor(&ui, MOUSE_CURSOR_RESIZE_ALL);
            sn_tip(&ui, "drag the point about - right-click to hold it or remove it");
        } else {
            sn_tip(&ui, "%s effects: click to add a point, Shift-drag to mark a "
                        "stretch, right-click for shapes",
                   ht.name.c_str());
        }
    } else if (inside && m.y < r.y + RULER_H) {
        sn_tip(&ui, "drag to scrub");
    }
}

} /* namespace sn */
