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
    LANE_H = 56,
    LANE_GAP = 2,
    EDGE_GRAB = 7,       /* how close to an edge counts as the edge */
    FADE_GRAB = 12
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

static Rectangle lane_rect(const App &a, int idx)
{
    const float top = a.rTimeline.y + RULER_H + 1;
    return Rectangle{a.rTimeline.x + HEAD_W,
                     top + (float)idx * (LANE_H + LANE_GAP),
                     a.rTimeline.width - HEAD_W, LANE_H};
}

static Rectangle head_rect(const App &a, int idx)
{
    Rectangle l = lane_rect(a, idx);
    return Rectangle{a.rTimeline.x, l.y, HEAD_W, l.height};
}

static Rectangle clip_rect(const App &a, int idx, const Clip &c)
{
    Rectangle l = lane_rect(a, idx);
    const float x0 = a.xAt(c.pos), x1 = a.xAt(c.end());
    return Rectangle{x0, l.y + 2, std::max(2.0f, x1 - x0), l.height - 4};
}

/* Which track lane a y falls in, or -1. */
static int track_at(const App &a, float y)
{
    for (size_t i = 0; i < a.proj.tracks.size(); i++) {
        Rectangle l = lane_rect(a, (int)i);
        if (y >= l.y - LANE_GAP * 0.5f && y < l.y + l.height + LANE_GAP * 0.5f) return (int)i;
    }
    return -1;
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

static void draw_clip(App &a, int idx, const Clip &c, bool video, bool hot)
{
    sn_ui &ui = a.ui;
    Rectangle r = clip_rect(a, idx, c);
    Rectangle lane = lane_rect(a, idx);

    /* Off-screen either side: cheap to skip and there can be hundreds. */
    if (r.x > lane.x + lane.width || r.x + r.width < lane.x) return;

    const bool isSel = a.selected(ClipRef{idx, c.id});
    const Color body = video ? (isSel ? SN_CLIP_V_HI : SN_CLIP_V)
                             : (isSel ? SN_CLIP_A_HI : SN_CLIP_A);
    const Color edge = video ? SN_CLIP_V_EDGE : SN_CLIP_A_EDGE;

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
    if (!video && r.width > 12) {
        /* A flat line with a bulge, not the real waveform: drawing that means
         * decoding the whole file, and the clip's length and position are
         * what the eye is actually using here. */
        const float mid = r.y + r.height * 0.6f;
        for (float x = r.x + 3; x < r.x + r.width - 3; x += 3) {
            const float ph = std::sin((x - r.x) * 0.11f) * 0.5f + 0.5f;
            const float h = (r.height * 0.28f) * (0.35f + 0.65f * ph);
            DrawLineEx(Vector2{x, mid - h}, Vector2{x, mid + h}, 1.5f,
                       Color{(unsigned char)(edge.r), (unsigned char)(edge.g),
                             (unsigned char)(edge.b), 160});
        }
    }

    /* Fades, drawn as the ramp they are. */
    if (c.fadeIn > 0) {
        const float w = (float)(c.fadeIn * a.zoom);
        DrawTriangle(Vector2{r.x, r.y}, Vector2{r.x, r.y + r.height},
                     Vector2{r.x + w, r.y}, Color{0, 0, 0, 120});
    }
    if (c.fadeOut > 0) {
        const float w = (float)(c.fadeOut * a.zoom);
        DrawTriangle(Vector2{r.x + r.width, r.y}, Vector2{r.x + r.width - w, r.y},
                     Vector2{r.x + r.width, r.y + r.height}, Color{0, 0, 0, 120});
    }

    /* The fade handles: small squares in the top corners. Only when the clip
     * is wide enough that they are not the whole clip. */
    if (r.width > 40 && (hot || isSel)) {
        DrawRectangle((int)(r.x + (float)(c.fadeIn * a.zoom)) - 3, (int)r.y + 1, 6, 6,
                      SN_TEXT);
        DrawRectangle((int)(r.x + r.width - (float)(c.fadeOut * a.zoom)) - 3, (int)r.y + 1,
                      6, 6, SN_TEXT);
    }

    /* The name, and how long it is. Both go in a strip along the bottom
     * rather than over the picture at the head of the clip - text on top of a
     * thumbnail is readable in neither direction. */
    const BinItem *b = a.proj.item(c.source);
    if (r.width > 46) {
        Rectangle strip = {r.x + 1, r.y + r.height - 15, r.width - 2, 14};
        DrawRectangleRec(strip, Color{0, 0, 0, 90});

        sn_text_clip(&ui, SN_F_TINY, b ? b->info.name.c_str() : "?", strip.x + 4,
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

/* ------------------------------------------------------------------ *
 * The ruler
 * ------------------------------------------------------------------ */

static void draw_ruler(App &a, Rectangle r)
{
    sn_ui &ui = a.ui;
    Rectangle ruler = {r.x + HEAD_W, r.y, r.width - HEAD_W, RULER_H};
    DrawRectangleRec(Rectangle{r.x, r.y, r.width, RULER_H}, SN_PANEL);

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

    for (size_t i = 0; i < a.proj.tracks.size(); i++) {
        Track &t = a.proj.tracks[i];
        Rectangle h = head_rect(a, (int)i);
        if (h.y > a.rTimeline.y + a.rTimeline.height) break;

        DrawRectangleRec(h, SN_PANEL);
        DrawLine((int)(h.x + h.width) - 1, (int)h.y, (int)(h.x + h.width) - 1,
                 (int)(h.y + h.height), SN_BORDER);

        sn_text_spaced(&ui, SN_F_SMALL, t.name.c_str(), h.x + 8, h.y + 6,
                       t.muted ? SN_EDGE : SN_TEXT);

        const float by = h.y + h.height - 24;
        Rectangle b1 = {h.x + 8, by, 20, 18};
        Rectangle b2 = {h.x + 32, by, 20, 18};

        const int id = 3000 + (int)i * 8;
        if (t.kind == TRACK_VIDEO) {
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
    }
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

    /* --- lanes --- */
    for (size_t i = 0; i < a.proj.tracks.size(); i++) {
        Rectangle l = lane_rect(a, (int)i);
        if (l.y > r.y + r.height) break;
        DrawRectangleRec(l, a.proj.tracks[i].kind == TRACK_VIDEO
                                ? Color{0x0e, 0x14, 0x0c, 255}
                                : Color{0x0c, 0x12, 0x0a, 255});
        if (a.proj.tracks[i].locked)
            for (float x = l.x; x < l.x + l.width; x += 10)
                DrawLineEx(Vector2{x, l.y}, Vector2{x + l.height, l.y + l.height}, 1,
                           Color{0x2a, 0x3a, 0x1e, 60});
    }

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
            Rectangle cr = clip_rect(a, overTrack, c);
            if (!CheckCollisionPointRec(m, cr)) continue;

            overClip = ClipRef{overTrack, c.id};

            const bool wide = cr.width > 24;
            if (wide && m.x < cr.x + EDGE_GRAB) overWhat = DRAG_TRIM_IN;
            else if (wide && m.x > cr.x + cr.width - EDGE_GRAB) overWhat = DRAG_TRIM_OUT;
            else if (m.y < cr.y + 10 &&
                     std::fabs(m.x - (cr.x + (float)(c.fadeIn * a.zoom))) < FADE_GRAB)
                overWhat = DRAG_FADE_IN;
            else if (m.y < cr.y + 10 &&
                     std::fabs(m.x - (cr.x + cr.width - (float)(c.fadeOut * a.zoom))) <
                         FADE_GRAB)
                overWhat = DRAG_FADE_OUT;
            else overWhat = DRAG_CLIP;
            break;
        }
    }

    /* --- clips --- */
    for (size_t i = 0; i < a.proj.tracks.size(); i++) {
        const Track &t = a.proj.tracks[i];
        if (lane_rect(a, (int)i).y > r.y + r.height) break;
        for (const Clip &c : t.clips)
            draw_clip(a, (int)i, c, t.kind == TRACK_VIDEO,
                      overClip.track == (int)i && overClip.clip == c.id);
    }

    draw_heads(a);
    draw_ruler(a, r);

    /* --- starting a drag --- */
    if (inside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && a.drag == DRAG_NONE) {
        if (m.y < r.y + RULER_H && m.x > r.x + HEAD_W) {
            a.drag = DRAG_SCRUB;
            a.follow = false;
        } else if (overClip.ok() && !a.proj.tracks[overClip.track].locked) {
            const Clip *c = a.proj.clip(overClip);
            a.select(overClip, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
            a.drag = overWhat;
            a.dragClip = overClip;
            a.dragTrack = overClip.track;
            a.dragFrom = m;
            a.dragMoved = false;
            a.dragGrab = a.timeAt(m.x) - (c ? c->pos : 0);
            a.dragStart = c ? c->pos : 0;
        } else if (overTrack >= 0) {
            a.clearSel();
            /* Clicking empty timeline moves the playhead there, which is
             * what everyone expects and saves a trip to the ruler. */
            a.playhead = std::max(0.0, snap_time(a, a.timeAt(m.x), nullptr));
            a.player.seek(a.playhead);
            a.drag = DRAG_SCRUB;
            a.follow = false;
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
        case DRAG_FADE_IN:
        case DRAG_FADE_OUT: {
            Clip *c = a.proj.clip(a.dragClip);
            if (!c) break;
            double f = a.drag == DRAG_FADE_IN ? a.timeAt(m.x) - c->pos
                                              : c->end() - a.timeAt(m.x);
            f = std::max(0.0, std::min(f, c->dur()));
            (a.drag == DRAG_FADE_IN ? c->fadeIn : c->fadeOut) = f;
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
                const bool audioLane = a.proj.tracks[overTrack].kind == TRACK_AUDIO;
                placeItem(a.proj, a.dragBin, t, audioLane ? NO_TRACK : overTrack,
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
        if (a.dragMoved && a.drag != DRAG_SCRUB) a.changed();
        a.drag = DRAG_NONE;
    }

    /* --- right-click --- */
    if (inside && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && overClip.ok()) {
        a.select(overClip, false);
        static const char *items[] = {"split here", "delete", "delete and close the gap",
                                      "-", "mute", "clear fades"};
        sn_menu_open(&ui, m, items, 6, 100);
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
                               : overWhat == DRAG_TRIM_OUT ? "drag to trim the end"
                               : overWhat == DRAG_FADE_IN  ? "drag the fade in"
                               : overWhat == DRAG_FADE_OUT ? "drag the fade out"
                                                           : "drag to move it";
            sn_tip(&ui, "%s  %s of %s  -  %s", b->info.name.c_str(),
                   fmtTime(c->dur()).c_str(), fmtTime(b->info.duration).c_str(), verb);
        }
    } else if (inside && m.y < r.y + RULER_H) {
        sn_tip(&ui, "drag to scrub");
    }
}

} /* namespace sn */
