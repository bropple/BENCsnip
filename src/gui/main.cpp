/*
 * BENCsnip - the window
 *
 * Layout, the toolbar, the preview, the keyboard, and the small number of
 * actions that more than one of those can start. The panes are in their own
 * files; what is here is the arrangement and the glue.
 *
 *   +--------------------------------------------------+
 *   | toolbar                                          |
 *   +-----------+--------------------------------------+
 *   | media bin | preview                              |
 *   |           +--------------------------------------+
 *   |           | transport                            |
 *   +-----------+--------------------------------------+
 *   | timeline                                         |
 *   +--------------------------------------------------+
 *   | status                                           |
 *   +--------------------------------------------------+
 *
 * The timeline spans the width because that is the axis it is about, and
 * every editor that puts it in a column has been argued with ever since.
 */

#include "sn_app.h"
#include "sn_appmenu.h"
#include "sn_embed.h"
#include "sn_filedlg.h"
#include "sn_version.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sn {

enum { TOOLBAR_H = 38, TRANSPORT_H = 34, STATUS_H = 22, BIN_W = 264 };

/* Everything ffmpeg is likely to be handed, for the file dialogs. Not a limit
 * on what can be dropped - a drop is given straight to libav, which knows
 * better than any list.
 *
 * That difference was a bug rather than a design: a GIF dropped on the window
 * worked and the same GIF was invisible in the open dialog, because the list
 * had no gif in it. Pictures are here for the same reason - libav decodes
 * them, the renderer composites them, and a still gets a length of its own so
 * it can be trimmed like anything else. See MediaInfo::still. */
static const char *MEDIA_EXTS =
    "mp4 mov mkv webm avi m4v mpg mpeg wmv flv ts m2ts mts 3gp ogv "
    "mp3 wav flac m4a aac ogg opus wma aiff aif "
    "gif png jpg jpeg webp bmp tif tiff tga";

/* ------------------------------------------------------------------ *
 * App
 * ------------------------------------------------------------------ */

void App::say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    status = buf;
    statusAt = GetTime();
    statusBad = false;
}

void App::complain(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    status = buf;
    statusAt = GetTime();
    statusBad = true;
}

void App::changed(bool minor)
{
    rev++;
    proj.dirty = true;
    if (!minor) hist.commit(proj);
    player.setEnd(proj.duration());
}

bool App::selected(const ClipRef &r) const
{
    for (const ClipRef &s : sel)
        if (s == r) return true;
    return false;
}

void App::select(const ClipRef &r, bool add)
{
    if (!add) sel.clear();
    if (!selected(r)) sel.push_back(r);

    /* A clip's linked partner comes with it. They move together, they trim
     * together and deleting one deletes both, so showing only one of them as
     * selected would be the interface lying about what the next key does. */
    const Clip *c = proj.clip(r);
    if (c && c->link) {
        for (size_t ti = 0; ti < proj.tracks.size(); ti++)
            for (const Clip &x : proj.tracks[ti].clips) {
                ClipRef o{(int)ti, x.id};
                if (x.link == c->link && !selected(o)) sel.push_back(o);
            }
    }

    selBin = 0;
}

void App::clearSel()
{
    sel.clear();
}

/* ------------------------------------------------------------------ *
 * Actions
 * ------------------------------------------------------------------ */

void zoomTo(App &a, double centre, double newZoom)
{
    /* Between one pixel a second - four hours across a wide window - and two
     * hundred pixels a frame. Past either end a timeline stops being one. */
    newZoom = std::max(1.0, std::min(newZoom, 4000.0));

    /* `centre` must stay where it is on screen. It sits (centre - scroll) *
     * zoom pixels from the left edge of the lanes, so holding that product
     * fixed across the change in zoom gives the new scroll. */
    const double pixels = (centre - a.scroll) * a.zoom;
    a.zoom = newZoom;
    a.scroll = std::max(0.0, centre - pixels / newZoom);
}

void zoomFit(App &a)
{
    const double d = std::max(1.0, a.proj.duration());
    const double w = std::max(200.0, (double)a.rTimeline.width - 104.0);
    a.zoom = std::max(1.0, w / (d * 1.05));
    a.scroll = 0;
}

void doSplit(App &a)
{
    const int n = splitAt(a.proj, a.playhead);
    if (n) {
        a.changed();
        a.say("cut %d clip%s at %s", n, n == 1 ? "" : "s", fmtTime(a.playhead).c_str());
    } else {
        a.complain("nothing under the playhead to cut");
    }
}

void doDelete(App &a, bool ripple)
{
    if (a.sel.empty()) {
        /* Nothing selected, but the playhead is sitting in a hole: close it.
         * That is what someone pressing delete over a gap means, and the
         * alternative is telling them off for it. */
        if (closeGap(a.proj, a.playhead)) {
            a.changed();
            a.say("closed the gap at %s", fmtTime(a.playhead).c_str());
        } else {
            a.complain("nothing selected");
        }
        return;
    }

    auto count = [&] {
        int n = 0;
        for (const Track &t : a.proj.tracks) n += (int)t.clips.size();
        return n;
    };
    const int before = count();

    /* Removing one takes its linked partner with it, and the partner is in
     * the selection too - so a second attempt at it finds nothing and says
     * so. Counting what actually went is simpler than reasoning about which
     * of the two did it. */
    std::vector<ClipRef> victims = a.sel;
    for (const ClipRef &r : victims) removeClip(a.proj, r, ripple);

    const int n = before - count();
    a.sel.clear();
    a.changed();
    a.say("deleted %d clip%s%s", n, n == 1 ? "" : "s", ripple ? " and closed the gap" : "");
}

void doImport(App &a, const std::string &path, bool place)
{
    std::string err;

    /* A project file dropped on the window opens it rather than being
     * imported as media, which is what dropping one obviously means. */
    if (path.size() > 9 && path.compare(path.size() - 9, 9, ".bencsnip") == 0) {
        Project p;
        /* loadProject fills `err` on success too, when it had to leave
         * something behind - a fade from a project written before the effects
         * lane existed. Worth saying: a project that comes back looking
         * different should say why rather than leave somebody to wonder. */
        if (loadProject(&p, path, &err)) {
            a.proj = p;
            a.hist.reset(a.proj);
            a.thumbs.clear();
            a.playhead = 0;
            a.sel.clear();
            a.changed(true);
            zoomFit(a);
            if (err.empty()) a.say("opened %s", GetFileName(path.c_str()));
            else a.complain("opened %s - %s", GetFileName(path.c_str()), err.c_str());
        } else {
            a.complain("%s", err.c_str());
        }
        return;
    }

    const int id = importFile(a.proj, path, &err);
    if (!id) { a.complain("%s", err.c_str()); return; }

    if (place) a.playhead = placeItem(a.proj, id, a.playhead);
    a.changed();

    const BinItem *b = a.proj.item(id);
    if (b) a.say("%s  %s", b->info.name.c_str(), fmtTime(b->info.duration).c_str());
}

/* ------------------------------------------------------------------ *
 * The preview
 * ------------------------------------------------------------------ */

void previewPane(App &a, Rectangle r)
{
    sn_ui &ui = a.ui;
    a.rPreview = r;

    Rectangle view = {r.x, r.y, r.width, r.height - TRANSPORT_H};
    DrawRectangleRec(view, SN_BG);

    /* The preview is the canvas, at the canvas's own shape.
     *
     * It used to ask for whatever rectangle the pane happened to be, and that
     * was fine while every layer filled the frame - one picture fitted into
     * any box looks the same. It stopped being fine the moment tracks could
     * be placed on the canvas: "the right half" of a 16:9 project is not the
     * right half of a 3:1 pane, so a side-by-side layout previewed as
     * something the export would never produce.
     *
     * So the size asked for has the project's aspect, scaled to fit the pane,
     * and what comes back is drawn at exactly that size. */
    const double par = a.proj.height > 0
                           ? (double)a.proj.width / a.proj.height : 16.0 / 9.0;

    double fitW = view.width - 16, fitH = (view.width - 16) / par;
    if (fitH > view.height - 16) {
        fitH = view.height - 16;
        fitW = fitH * par;
    }

    /* Even, and never smaller than something a scaler will accept. Snapping
     * to a multiple of sixteen would be kinder to the decoder but would bend
     * the aspect, which is the thing this is here to keep. */
    const int pw = std::max(64, ((int)fitW) & ~1);
    const int ph = std::max(36, ((int)fitH) & ~1);

    a.player.setPreviewSize(pw, ph);

    VideoFrame f;
    if (a.player.takeFrame(&f) && f.valid()) {
        if (a.preview.id == 0 || a.previewW != f.w || a.previewH != f.h) {
            if (a.preview.id) UnloadTexture(a.preview);
            Image img;
            img.data = f.rgba.data();
            img.width = f.w;
            img.height = f.h;
            img.mipmaps = 1;
            img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            a.preview = LoadTextureFromImage(img);
            a.previewW = f.w;
            a.previewH = f.h;
        } else {
            UpdateTexture(a.preview, f.rgba.data());
        }
    }

    /* Where the canvas is on screen. Worked out once and used by both the
     * drawing and the dragging below - two copies of this arithmetic is two
     * chances for what you drag to be somewhere other than what you see. */
    const Rectangle dst = {std::floor(view.x + (view.width - a.previewW) * 0.5f),
                           std::floor(view.y + (view.height - a.previewH) * 0.5f),
                           (float)a.previewW, (float)a.previewH};

    if (a.preview.id && a.proj.duration() > 0) {
        /* Centred at its own size: it was rendered at the canvas's aspect, so
         * there is nothing left to letterbox. The frame drawn round it is
         * where the canvas ends, which is worth seeing when a track has been
         * pushed off the edge of it. */
        DrawTexturePro(a.preview, Rectangle{0, 0, (float)a.previewW, (float)a.previewH},
                       dst, Vector2{0, 0}, 0, WHITE);
        DrawRectangleLinesEx(dst, 1, SN_BORDER);
    } else {
        /* Empty: the mascot and one line about what to do. */
        static int line = 0;
        static double lineAt = 0;
        if (GetTime() - lineAt > 20.0) { lineAt = GetTime(); line++; }

        tarr(a, Vector2{view.x + view.width * 0.5f, view.y + view.height * 0.5f - 20}, 46,
             TARR_IDLE);
        sn_text_center(&ui, SN_F_BODY, tarrLine(line), view.x + view.width * 0.5f,
                       view.y + view.height * 0.5f + 42, SN_DIM);
    }

    /* --- the layer under the pointer, and dragging it -----------------
     *
     * The layout window has the numbers; this has the picture. Everything
     * here works in canvas units and converts at the edges, because the
     * preview is drawn at whatever size fits the pane and the numbers it
     * writes have to mean the same thing at any of them.
     * ------------------------------------------------------------------ */
    if (a.preview.id && a.proj.duration() > 0) {
        const float sc = (float)a.previewW / (float)std::max(1, a.proj.width);
        const Vector2 m = GetMousePosition();

        /* Where a track's picture is on the canvas, in canvas units. The same
         * arithmetic the renderer does - if these two ever disagree, what you
         * drag is not what gets written. */
        auto layerRect = [&](const Track &t) {
            double sw = 16, sh = 9;
            for (const Clip &c : t.clips) {
                const BinItem *b = a.proj.item(c.source);
                if (b && b->info.hasVideo) {
                    sw = b->info.dispW() * std::max(0.02, 1.0 - t.cropL - t.cropR);
                    sh = b->info.dispH() * std::max(0.02, 1.0 - t.cropT - t.cropB);
                    break;
                }
            }
            const double W = a.proj.width, H = a.proj.height;
            const double boxW = W * std::max(0.01, t.scaleX);
            const double boxH = H * std::max(0.01, t.scaleY);

            double lw = boxW, lh = boxH;
            if (!t.stretch) {
                const double sa = sw / sh, ba = boxW / boxH;
                lw = sa > ba ? boxW : boxH * sa;
                lh = sa > ba ? boxW / sa : boxH;
            }
            return Rectangle{(float)((W - lw) * 0.5 * (1.0 + t.x)),
                             (float)((H - lh) * 0.5 * (1.0 + t.y)), (float)lw,
                             (float)lh};
        };

        auto toScreen = [&](Rectangle c) {
            return Rectangle{dst.x + c.x * sc, dst.y + c.y * sc, c.width * sc,
                             c.height * sc};
        };

        const bool overView = CheckCollisionPointRec(m, view) && !sn_ui_blocked(&a.ui);

        /* --- captions ------------------------------------------------
         *
         * A caption is placed per clip rather than per track, so this cannot
         * reuse the layer handles above: what is being moved is one clip's
         * style, and two captions on one track are two different positions.
         * It is also rotatable, which nothing else on this canvas is, so the
         * box is four corners rather than a rectangle and every hit test here
         * is against a quad.
         *
         * It runs before the layer code so that a caption in front of a video
         * layer is what a click on it picks. Pressing on one sets a.drag
         * immediately - even for a click that only selects - which is what
         * stops the layer code below from also grabbing the picture
         * underneath, and is what that code does for its own layers.
         *
         * The geometry comes from textBox() in the core, which is the same
         * measurement the renderer fills. Working it out again here would be
         * two answers to one question, and the one the mouse got would be the
         * wrong one.
         * ------------------------------------------------------------------ */
        {
            const int CW = a.proj.width, CH = a.proj.height;

            auto capQuad = [&](const TextStyle &st, Vector2 out[4]) {
                double k[8];
                if (!textBox(st, CW, CH, k)) return false;
                for (int i = 0; i < 4; i++)
                    out[i] = Vector2{dst.x + (float)k[i * 2] * sc,
                                     dst.y + (float)k[i * 2 + 1] * sc};
                return true;
            };

            auto centreOf = [](const Vector2 q[4]) {
                return Vector2{(q[0].x + q[1].x + q[2].x + q[3].x) * 0.25f,
                               (q[0].y + q[1].y + q[2].y + q[3].y) * 0.25f};
            };

            /* Inside a convex quad is on the same side of all four edges. */
            auto inQuad = [](Vector2 p, const Vector2 q[4]) {
                int pos = 0, neg = 0;
                for (int i = 0; i < 4; i++) {
                    const Vector2 &u = q[i], &v = q[(i + 1) & 3];
                    const float d =
                        (p.x - u.x) * (v.y - u.y) - (p.y - u.y) * (v.x - u.x);
                    if (d > 0.0f) pos++;
                    else if (d < 0.0f) neg++;
                }
                return pos == 0 || neg == 0;
            };

            /* What is selected, taken from the timeline's selection so that
             * the two panes cannot hold different ideas of it. Only a caption
             * the playhead is actually inside gets handles: drawing them
             * around something not on screen would be furniture over a frame
             * it has nothing to do with. */
            Clip *cap = nullptr;
            for (const ClipRef &r0 : a.sel) {
                const Track *t0 = a.proj.track(r0.track);
                if (!t0 || t0->kind != TRACK_TEXT || t0->locked) continue;
                Clip *c0 = a.proj.clip(r0);
                if (!c0 || !c0->covers(a.playhead)) continue;
                cap = c0;
                a.textClip = r0;
                break;
            }

            /* --- everything under the pointer, front to back ---
             *
             * Front to back because a click picks what can be seen, and the
             * list is stored back-first. Captions and pictures go in the same
             * list, in the one order the compositor uses, so that Tab can walk
             * through the lot: a caption over a video layer over another video
             * layer is three things at the same point on screen, and only the
             * front one is reachable with the mouse alone.
             */
            struct Pick {
                bool text;
                int track;
                int clip;      /* text only */
            };
            std::vector<Pick> under;

            for (int i = (int)a.proj.tracks.size() - 1; i >= 0; i--) {
                const Track &t0 = a.proj.tracks[i];
                if (t0.muted || t0.locked) continue;

                if (t0.kind == TRACK_TEXT) {
                    const Clip *c0 = t0.at(a.playhead);
                    if (!c0 || c0->text.text.empty()) continue;
                    Vector2 qq[4];
                    if (capQuad(c0->text, qq) && inQuad(m, qq))
                        under.push_back(Pick{true, i, c0->id});
                } else if (t0.kind == TRACK_VIDEO) {
                    if (!t0.at(a.playhead)) continue;
                    if (CheckCollisionPointRec(m, toScreen(layerRect(t0))))
                        under.push_back(Pick{false, i, 0});
                }
            }

            ClipRef capHit;
            for (const Pick &pk : under)
                if (pk.text) { capHit = ClipRef{pk.track, pk.clip}; break; }

            /* --- Tab: the next one down --- */
            if (overView && !under.empty() && IsKeyPressed(KEY_TAB) &&
                a.drag == DRAG_NONE) {
                /* Where the current selection sits in that list, so Tab moves
                 * one layer back rather than always to the front. */
                int at = -1;
                for (size_t i = 0; i < under.size(); i++) {
                    if (under[i].text) {
                        if (a.textClip.track == under[i].track &&
                            a.textClip.clip == under[i].clip && !a.sel.empty())
                            at = (int)i;
                    } else if (a.layoutTrack == under[i].track) {
                        at = (int)i;
                    }
                }

                const Pick &next = under[(size_t)((at + 1) % (int)under.size())];
                if (next.text) {
                    a.layoutTrack = -1;
                    a.sel.clear();
                    a.sel.push_back(ClipRef{next.track, next.clip});
                    a.textClip = a.sel[0];
                    a.say("%s", a.proj.tracks[next.track].name.c_str());
                } else {
                    a.sel.clear();
                    a.layoutTrack = next.track;
                    a.say("%s", a.proj.tracks[next.track].name.c_str());
                }
            }

            /* --- the handles --- */
            Vector2 q[4] = {};
            Vector2 rot = {0, 0};
            bool haveBox = false;

            if (cap && capQuad(cap->text, q)) {
                haveBox = true;

                /* The turn handle stands off the top edge along the box's own
                 * up direction, so it follows the caption round rather than
                 * staying north of it and crossing the box at 180 degrees. */
                const Vector2 topMid = {(q[0].x + q[1].x) * 0.5f, (q[0].y + q[1].y) * 0.5f};
                const Vector2 botMid = {(q[2].x + q[3].x) * 0.5f, (q[2].y + q[3].y) * 0.5f};
                float ux = topMid.x - botMid.x, uy = topMid.y - botMid.y;
                const float ul = std::sqrt(ux * ux + uy * uy);
                if (ul > 0.001f) { ux /= ul; uy /= ul; }
                rot = Vector2{topMid.x + ux * 22.0f, topMid.y + uy * 22.0f};

                /* Kept inside the pane. A caption against the top of the
                 * frame puts its handle above the canvas, and with a tall
                 * caption - three lines, or two with a blank between them -
                 * that lands in the toolbar, where the preview never sees the
                 * click and the caption cannot be turned at all.
                 *
                 * Clamped rather than flipped to the other side: a box that
                 * tall has no room underneath either, and a handle that moves
                 * to the far end of the thing it turns is a handle nobody
                 * will look for. */
                const float pad = 10.0f;
                rot.x = std::max(view.x + pad, std::min(view.x + view.width - pad, rot.x));
                rot.y = std::max(view.y + pad, std::min(view.y + view.height - pad, rot.y));

                for (int i = 0; i < 4; i++)
                    DrawLineEx(q[i], q[(i + 1) & 3], 1.0f, SN_ACCENT);
                DrawLineEx(topMid, rot, 1.0f, SN_ACCENT);

                /* Big enough to aim at. The first version drew nine pixel
                 * squares with a seven pixel reach, which is a small target
                 * on a large canvas and smaller still on a display that is
                 * counting in points while the eye is counting in pixels. */
                const float k = 6.0f;
                for (int i = 0; i < 4; i++) {
                    DrawRectangleRec(Rectangle{q[i].x - k, q[i].y - k, k * 2, k * 2}, SN_BG);
                    DrawRectangleLinesEx(Rectangle{q[i].x - k, q[i].y - k, k * 2, k * 2}, 1,
                                         SN_ACCENT);
                }
                DrawCircleV(rot, 6.0f, SN_BG);
                DrawCircleLinesV(rot, 6.0f, SN_ACCENT);
            }

            auto near = [](Vector2 p, Vector2 t, float r) {
                return std::fabs(p.x - t.x) <= r && std::fabs(p.y - t.y) <= r;
            };

            /* --- starting --- */
            if (overView && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                a.drag == DRAG_NONE) {
                int corner = -1;
                bool onRot = false;
                if (haveBox) {
                    /* The ball first. Clamping it into the pane can park it on
                     * top of a corner handle, and when the two overlap the one
                     * that was moved to get there is the one being aimed at. */
                    onRot = near(m, rot, 10.0f);
                    if (!onRot)
                        for (int i = 0; i < 4; i++)
                            if (near(m, q[i], 9.0f)) corner = i;
                }

                if (haveBox && (corner >= 0 || onRot)) {
                    a.drag = onRot ? DRAG_TEXT_ROT : DRAG_TEXT_SIZE;
                    a.textHandle = corner;
                    a.textGrab = cap->text;
                    a.layerFrom = m;
                    a.dragMoved = false;

                    const Vector2 ctr = centreOf(q);
                    a.textRotGrab = std::atan2(m.y - ctr.y, m.x - ctr.x);
                } else if (capHit.ok()) {
                    a.sel.clear();
                    a.sel.push_back(capHit);
                    a.textClip = capHit;
                    a.layoutTrack = -1;         /* one thing selected at a time */

                    cap = a.proj.clip(capHit);
                    if (cap) {
                        a.drag = DRAG_TEXT;
                        a.textHandle = -1;
                        a.textGrab = cap->text;
                        a.layerFrom = m;
                        a.dragMoved = false;
                    }
                }
            }

            /* --- carrying on --- */
            if (a.drag == DRAG_TEXT || a.drag == DRAG_TEXT_SIZE ||
                a.drag == DRAG_TEXT_ROT) {
                Clip *c1 = a.proj.clip(a.textClip);

                if (!c1) {
                    a.drag = DRAG_NONE;
                } else {
                    if (!a.dragMoved && std::fabs(m.x - a.layerFrom.x) +
                                                std::fabs(m.y - a.layerFrom.y) >= 2.0f)
                        a.dragMoved = true;

                    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                        if (a.dragMoved) a.changed();
                        a.drag = DRAG_NONE;
                        a.textHandle = -1;
                    } else if (a.dragMoved) {
                        /* Every frame works from the style as it was when the
                         * drag began, not from the style as it is now. Reading
                         * back what the last frame wrote accumulates its own
                         * rounding, and a caption dragged slowly across the
                         * canvas arrives somewhere the pointer is not. */
                        Vector2 g[4];
                        if (capQuad(a.textGrab, g)) {
                            const Vector2 ctr = centreOf(g);
                            const double lw = std::hypot(g[1].x - g[0].x, g[1].y - g[0].y) / sc;
                            const double lh = std::hypot(g[2].x - g[1].x, g[2].y - g[1].y) / sc;

                            TextStyle st = a.textGrab;

                            if (a.drag == DRAG_TEXT) {
                                /* The centre moves with the pointer, and then
                                 * becomes x and y again: fractions of the space
                                 * left over, which is what makes -1 mean "hard
                                 * against that edge" at any size. */
                                const double ncx =
                                    (ctr.x - dst.x) / sc + (m.x - a.layerFrom.x) / sc;
                                const double ncy =
                                    (ctr.y - dst.y) / sc + (m.y - a.layerFrom.y) / sc;

                                const double freeW = CW - lw, freeH = CH - lh;
                                st.x = std::fabs(freeW) < 1.0
                                           ? 0.0
                                           : (ncx - lw * 0.5) / (freeW * 0.5) - 1.0;
                                st.y = std::fabs(freeH) < 1.0
                                           ? 0.0
                                           : (ncy - lh * 0.5) / (freeH * 0.5) - 1.0;
                                st.x = std::max(-4.0, std::min(4.0, st.x));
                                st.y = std::max(-4.0, std::min(4.0, st.y));
                            } else if (a.drag == DRAG_TEXT_SIZE) {
                                /* How much further from the centre the pointer
                                 * is than the corner it grabbed. Distance
                                 * rather than an axis, because the box may be
                                 * turned and "wider" then has no screen
                                 * direction to mean. */
                                const int h = a.textHandle >= 0 ? a.textHandle : 0;
                                const double was = std::hypot(g[h].x - ctr.x, g[h].y - ctr.y);
                                const double now = std::hypot(m.x - ctr.x, m.y - ctr.y);
                                if (was > 1.0) {
                                    double f = now / was;
                                    f = std::max(0.05, std::min(20.0, f));
                                    st.size = std::max(0.005, std::min(2.0,
                                                                       a.textGrab.size * f));
                                }
                            } else {
                                double d = (std::atan2(m.y - ctr.y, m.x - ctr.x) -
                                            a.textRotGrab) *
                                           180.0 / 3.14159265358979323846;
                                double r1 = a.textGrab.rotation + d;

                                /* Shift steps by fifteen degrees; without it,
                                 * upright is sticky, because level is what
                                 * almost every caption wants and a mouse
                                 * cannot land on zero. */
                                const bool shiftDown = IsKeyDown(KEY_LEFT_SHIFT) ||
                                                       IsKeyDown(KEY_RIGHT_SHIFT);
                                if (shiftDown) r1 = std::round(r1 / 15.0) * 15.0;
                                else if (std::fabs(r1) < 2.0) r1 = 0.0;

                                while (r1 > 180.0) r1 -= 360.0;
                                while (r1 < -180.0) r1 += 360.0;
                                st.rotation = r1;
                            }

                            if (st != c1->text) {
                                c1->text = st;
                                a.changed(true);
                            }
                        }
                    }
                }
            }

            /* --- what the pointer says it will do --- */
            if (overView && a.drag == DRAG_NONE) {
                bool said = false;
                if (haveBox && near(m, rot, 10.0f)) {
                    sn_cursor(&a.ui, MOUSE_CURSOR_POINTING_HAND);
                    sn_tip(&a.ui, "drag to turn it - Shift steps by fifteen degrees");
                    said = true;
                }
                if (haveBox && !said) {
                    for (int i = 0; i < 4; i++)
                        if (near(m, q[i], 9.0f)) {
                            sn_cursor(&a.ui, i == 0 || i == 2 ? MOUSE_CURSOR_RESIZE_NWSE
                                                              : MOUSE_CURSOR_RESIZE_NESW);
                            sn_tip(&a.ui, "drag to resize the caption");
                            said = true;
                        }
                }
                if (!said && capHit.ok()) {
                    sn_cursor(&a.ui, MOUSE_CURSOR_RESIZE_ALL);
                    sn_tip(&a.ui, "drag the caption about. Double-click for the words");
                    said = true;
                }
            }

            /* Double-click opens the window with the words in it. */
            if (overView && capHit.ok() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                sn_double_click(&a.ui, 5300 + capHit.track)) {
                a.textClip = capHit;
                a.modal = MODAL_TEXT;
            }
        }

        /* Front to back, so a click picks what a person can actually see:
         * the list is stored back-first.
         *
         * A locked track is not here. The lock is what somebody sets once the
         * background is where they want it and they are placing things on top
         * of it, and a lock that holds on the timeline but not on the canvas
         * is worse than none - it is a promise kept in one window and broken
         * in the other. */
        int hit = -1;
        for (int i = (int)a.proj.tracks.size() - 1; i >= 0 && hit < 0; i--) {
            const Track &t = a.proj.tracks[i];
            if (t.kind != TRACK_VIDEO || t.muted || t.locked || !t.at(a.playhead)) continue;
            if (CheckCollisionPointRec(m, toScreen(layerRect(t)))) hit = i;
        }

        Track *sel = a.proj.track(a.layoutTrack);
        if (sel && (sel->kind != TRACK_VIDEO || sel->locked)) sel = nullptr;

        /* --- the handles of whatever is selected --- */
        Rectangle hs[8] = {};
        if (sel) {
            const Rectangle s2 = toScreen(layerRect(*sel));
            const float k = 5;
            const float xs[3] = {s2.x, s2.x + s2.width * 0.5f, s2.x + s2.width};
            const float ys[3] = {s2.y, s2.y + s2.height * 0.5f, s2.y + s2.height};
            const int hx[8] = {0, 1, 2, 2, 2, 1, 0, 0};
            const int hy[8] = {0, 0, 0, 1, 2, 2, 2, 1};
            for (int i = 0; i < 8; i++)
                hs[i] = Rectangle{xs[hx[i]] - k, ys[hy[i]] - k, k * 2, k * 2};

            DrawRectangleLinesEx(s2, 1, SN_ACCENT);
            for (int i = 0; i < 8; i++) {
                DrawRectangleRec(hs[i], SN_BG);
                DrawRectangleLinesEx(hs[i], 1, SN_ACCENT);
            }
        }

        /* --- starting a drag --- */
        if (overView && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && a.drag == DRAG_NONE) {
            int grabbed = -1;
            if (sel)
                for (int i = 0; i < 8; i++)
                    if (CheckCollisionPointRec(m, hs[i])) grabbed = i;

            if (grabbed >= 0) {
                a.drag = DRAG_LAYER_SIZE;
                a.layerHandle = grabbed;
                a.layerGrab = layerRect(*sel);
                a.layerFrom = m;
                a.dragMoved = false;
            } else if (hit >= 0) {
                a.layoutTrack = hit;
                a.drag = DRAG_LAYER;
                a.layerHandle = -1;
                a.layerGrab = layerRect(a.proj.tracks[hit]);
                a.layerFrom = m;

                /* The selection just changed, and everything below works on
                 * `sel` - which was resolved at the top of this block and is
                 * still the layer that was selected a moment ago.
                 *
                 * Leaving it stale is what made picking up a second layer
                 * throw the first one across the canvas: the drag would write
                 * the newly grabbed rectangle into the old track, which reads
                 * as one layer jumping to where the other one is. */
                sel = &a.proj.tracks[hit];
                a.dragMoved = false;
            } else {
                a.layoutTrack = -1;
                sel = nullptr;
            }
        }

        /* --- carrying it on --- */
        if ((a.drag == DRAG_LAYER || a.drag == DRAG_LAYER_SIZE) && sel) {
            /* Until the pointer has actually gone somewhere, this is a click
             * that selected a layer rather than a drag that moved one.
             * Writing the same numbers back would put a step on the undo
             * stack for having looked at something. */
            if (!a.dragMoved &&
                std::fabs(m.x - a.layerFrom.x) + std::fabs(m.y - a.layerFrom.y) >= 2.0f)
                a.dragMoved = true;

            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                /* Let go. Only a drag that changed something is worth an undo
                 * step; a click that merely picked a layer is not. */
                if (a.dragMoved) a.changed();
                a.drag = DRAG_NONE;
                a.layerHandle = -1;
            } else if (a.dragMoved) {
                const float dx = (m.x - a.layerFrom.x) / sc;
                const float dy = (m.y - a.layerFrom.y) / sc;

                Rectangle g = a.layerGrab;
                Rectangle n = g;

                if (a.drag == DRAG_LAYER) {
                    n.x = g.x + dx;
                    n.y = g.y + dy;
                } else {
                    /* Which edges this handle owns. 0..7 clockwise from the
                     * top left, so the corners move two and the sides one. */
                    const int h = a.layerHandle;
                    const bool left = (h == 0 || h == 6 || h == 7);
                    const bool right = (h == 2 || h == 3 || h == 4);
                    const bool top = (h == 0 || h == 1 || h == 2);
                    const bool bottom = (h == 4 || h == 5 || h == 6);

                    if (left) { n.x = g.x + dx; n.width = g.width - dx; }
                    if (right) { n.width = g.width + dx; }
                    if (top) { n.y = g.y + dy; n.height = g.height - dy; }
                    if (bottom) { n.height = g.height + dy; }

                    /* With the aspect locked, the box keeps the shape it had
                     * and the drag decides how big: whichever axis moved
                     * further wins, so a corner drag feels like one gesture
                     * rather than two arguing. */
                    if (!sel->stretch && g.width > 1 && g.height > 1) {
                        const float ar = g.width / g.height;
                        if (std::fabs(n.width / g.width - 1.0f) >
                            std::fabs(n.height / g.height - 1.0f))
                            n.height = n.width / ar;
                        else
                            n.width = n.height * ar;
                        if (left) n.x = g.x + g.width - n.width;
                        if (top) n.y = g.y + g.height - n.height;
                    }

                    n.width = std::max(8.0f, n.width);
                    n.height = std::max(8.0f, n.height);
                }

                /* Back into the numbers the model keeps. The scale is the box
                 * as a fraction of the canvas; the position is where it sits
                 * in the space left over, which is what makes -1 mean "hard
                 * against that edge" whatever size it is. */
                const double W = a.proj.width, H = a.proj.height;
                sel->scaleX = n.width / W;
                sel->scaleY = n.height / H;

                const double freeW = W - n.width, freeH = H - n.height;
                sel->x = std::fabs(freeW) < 1.0 ? 0.0 : (2.0 * n.x / freeW - 1.0);
                sel->y = std::fabs(freeH) < 1.0 ? 0.0 : (2.0 * n.y / freeH - 1.0);
                sel->x = std::max(-4.0, std::min(4.0, sel->x));
                sel->y = std::max(-4.0, std::min(4.0, sel->y));

                a.changed(true);
            }
        }

        /* --- what the pointer says it will do --- */
        if (overView && a.drag == DRAG_NONE) {
            int over = -1;
            if (sel)
                for (int i = 0; i < 8; i++)
                    if (CheckCollisionPointRec(m, hs[i])) over = i;

            if (over >= 0) {
                static const int shape[8] = {
                    MOUSE_CURSOR_RESIZE_NWSE, MOUSE_CURSOR_RESIZE_NS,
                    MOUSE_CURSOR_RESIZE_NESW, MOUSE_CURSOR_RESIZE_EW,
                    MOUSE_CURSOR_RESIZE_NWSE, MOUSE_CURSOR_RESIZE_NS,
                    MOUSE_CURSOR_RESIZE_NESW, MOUSE_CURSOR_RESIZE_EW};
                sn_cursor(&a.ui, shape[over]);
            } else if (hit >= 0) {
                sn_cursor(&a.ui, MOUSE_CURSOR_RESIZE_ALL);
                sn_tip(&a.ui, "drag %s about, or its handles to resize it. "
                              "Double-click for the numbers",
                       a.proj.tracks[hit].name.c_str());
            }
        }

        /* Double-click opens the window with the numbers in it. */
        if (overView && hit >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            sn_double_click(&a.ui, 5100 + hit)) {
            a.layoutTrack = hit;
            a.modal = MODAL_LAYOUT;
        }
    }

    if (a.player.struggling())
        sn_text(&ui, SN_F_TINY, "dropping frames - the export will still be exact",
                view.x + 8, view.y + 8, SN_AMBER);

    /* --- transport --- */
    Rectangle tb = {r.x, view.y + view.height, r.width, TRANSPORT_H};
    DrawRectangleRec(tb, SN_PANEL);
    sn_divider(tb.x, tb.y, tb.width);

    const double dur = a.proj.duration();
    float x = tb.x + 8;
    const float by = tb.y + 5;

    struct {
        sn_icon icon;
        const char *tip;
        int id;
    } btns[] = {{SN_I_START, "back to the start (Home)", 1},
                {SN_I_PREV, "previous cut (,)", 2},
                {a.player.playing() ? SN_I_PAUSE : SN_I_PLAY, "play or pause (Space)", 3},
                {SN_I_NEXT, "next cut (.)", 4},
                {SN_I_END, "to the end (End)", 5}};

    for (auto &b : btns) {
        Rectangle br = {x, by, 26, 24};
        if (sn_icon_button(&ui, 100 + b.id, br, b.icon, dur > 0, 0, b.tip)) {
            switch (b.id) {
            case 1: a.playhead = 0; a.player.seek(0); break;
            case 2: a.playhead = prevEdit(a.proj, a.playhead); a.player.seek(a.playhead); break;
            case 3: a.player.togglePlay(); a.follow = true; break;
            case 4: a.playhead = nextEdit(a.proj, a.playhead); a.player.seek(a.playhead); break;
            case 5: a.playhead = dur; a.player.seek(dur); break;
            }
        }
        x += 30;
    }

    /* Timecode: where we are, and how long it all is. */
    char tc[96];
    snprintf(tc, sizeof tc, "%s / %s", fmtTime(a.playhead, true, a.proj.fps).c_str(),
             fmtTime(dur, true, a.proj.fps).c_str());
    sn_text(&ui, SN_F_SMALL, tc, x + 8, tb.y + 9, SN_TEXT);

    /* Volume, and what is coming out. */
    float l = 0, rr = 0;
    a.player.levels(&l, &rr);
    Rectangle meter = {tb.x + tb.width - 66, tb.y + 9, 56, 6};
    DrawRectangleRec(meter, SN_WELL);
    DrawRectangleRec(Rectangle{meter.x, meter.y, meter.width * l, 2.5f},
                     l > 0.99f ? SN_ALERT : SN_ACCENT);
    DrawRectangleRec(Rectangle{meter.x, meter.y + 3.5f, meter.width * rr, 2.5f},
                     rr > 0.99f ? SN_ALERT : SN_ACCENT);

    Rectangle vol = {tb.x + tb.width - 66, tb.y + 18, 56, 12};
    float v = a.player.volume();
    if (sn_slider(&ui, 140, vol, &v)) a.player.setVolume(v);

    Rectangle spk = {tb.x + tb.width - 92, tb.y + 6, 22, 22};
    if (sn_icon_button(&ui, 141, spk, v > 0.001f ? SN_I_SPEAKER : SN_I_MUTE, 1, 0,
                       v > 0.001f ? "mute the preview" : "unmute the preview")) {
        static float last = 1.0f;
        if (v > 0.001f) { last = v; a.player.setVolume(0.0f); }
        else a.player.setVolume(last);
    }
}

/* ------------------------------------------------------------------ *
 * The toolbar
 * ------------------------------------------------------------------ */

static void save_project(App &a, bool askPath)
{
    std::string path = a.proj.path;

    if (path.empty() || askPath) {
        char buf[1024] = {0};
        const std::string suggest = a.proj.name == "untitled" ? "untitled.bencsnip" : a.proj.name;
        int rc = sn_save_dialog(GetWindowHandle(), "Save project", suggest.c_str(),
                                "BENCsnip project", "bencsnip", buf, sizeof buf);
        if (rc == SN_DLG_OK) path = buf;
        else if (rc == SN_DLG_UNAVAILABLE) {
            a.modal = MODAL_SAVE;
            fileDialogOpen(".", suggest);
            return;
        } else return;
    }

    if (path.find(".bencsnip") == std::string::npos) path += ".bencsnip";

    std::string err;
    if (saveProject(a.proj, path, &err)) {
        a.proj.path = path;
        a.proj.name = GetFileName(path.c_str());
        a.proj.dirty = false;
        a.say("saved %s", a.proj.name.c_str());
    } else {
        a.complain("%s", err.c_str());
    }
}

static void open_files(App &a, bool project)
{
    char buf[32768] = {0};
    int rc = sn_open_dialog(GetWindowHandle(), project ? "Open project" : "Add media", ".",
                            project ? "BENCsnip project" : "Media",
                            project ? "bencsnip" : MEDIA_EXTS, project ? 0 : 1, buf,
                            sizeof buf);

    if (rc == SN_DLG_UNAVAILABLE) {
        a.modal = project ? MODAL_LOAD : MODAL_OPEN;
        fileDialogOpen(".", "");
        return;
    }
    if (rc != SN_DLG_OK) return;

    /* Several paths come back newline separated. */
    const char *p = buf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        std::string one = nl ? std::string(p, nl - p) : std::string(p);
        if (!one.empty()) doImport(a, one, false);
        if (!nl) break;
        p = nl + 1;
    }
}

/* ------------------------------------------------------------------ *
 * The commands
 *
 * Everything below is reachable from more than one place - a toolbar button,
 * a key, and on macOS a menu - and each of those used to carry its own copy of
 * what the command does. Which is how the toolbar's undo came to leave the
 * player's end where it was while the keyboard's moved it: four lines written
 * twice, and only one of the copies learned about setEnd.
 *
 * One function per command, then, and the callers choose which to call and
 * nothing else. Each returns without complaint when there is nothing to do,
 * because a menu item is clickable whether or not it applies and a caller that
 * had to check first would be the duplication coming back in.
 * ------------------------------------------------------------------ */

static void cmd_new(App &a)
{
    if (a.proj.dirty) {
        a.confirmText = "Throw away the unsaved changes?";
        a.confirmTag = 2;
        a.modal = MODAL_CONFIRM;
        return;
    }
    a.proj = newProject();
    a.hist.reset(a.proj);
    a.thumbs.clear();
    a.playhead = 0;
    a.changed(true);
}

static void cmd_undo(App &a)
{
    if (!a.hist.undo(&a.proj)) return;
    a.rev++;
    a.sel.clear();
    a.player.setEnd(a.proj.duration());
    a.say("undone");
}

static void cmd_redo(App &a)
{
    if (!a.hist.redo(&a.proj)) return;
    a.rev++;
    a.sel.clear();
    a.player.setEnd(a.proj.duration());
    a.say("redone");
}

static void cmd_export(App &a)
{
    if (a.proj.duration() <= 0) return;
    exportDialogPrepare(a);
    a.modal = MODAL_EXPORT;
}

static void cmd_select_all(App &a)
{
    a.sel.clear();
    for (size_t i = 0; i < a.proj.tracks.size(); i++)
        for (const Clip &c : a.proj.tracks[i].clips)
            a.sel.push_back(ClipRef{(int)i, c.id});
    a.say("selected %d clips", (int)a.sel.size());
}

/* Put a caption on the timeline at the playhead, and open the window that
 * says what it reads.
 *
 * On a text track that is free for those five seconds, and on a new one when
 * none is - which is freeTrack, the same rule a dropped file already follows.
 * It used to reuse the frontmost text track whatever was on it, and a caption
 * added over a caption cut a hole in the one underneath, because that is what
 * dropping a clip on top of another means everywhere else in this program.
 * A caption is not something you meant to land on another one.
 *
 * Not a new track every time either: two captions ten seconds apart are the
 * next caption, not a second layer of them, and a row per caption is a
 * timeline nobody can read. A new track appears exactly when the time is
 * already taken, which is the case that was doing damage.
 *
 * Five seconds because a caption is read rather than watched, and because a
 * length that has to be trimmed is friendlier than one that has to be found. */
static void cmd_add_text(App &a)
{
    const double from = std::max(0.0, a.playhead);
    const double to = from + 5.0;

    const int track = a.proj.freeTrack(TRACK_TEXT, from, to);
    if (track < 0) return;

    Clip c;
    c.id = a.proj.newId();
    c.source = 0;
    c.in = 0.0;
    c.out = 5.0;
    c.pos = from;
    c.text.text = "Text";
    c.text.size = 0.12;
    c.text.y = 0.6;                       /* low, where a caption belongs */
    c.text.fill = 0xffffffffu;
    c.text.outline = 0x000000ffu;
    c.text.outlineWidth = 0.08;

    const Clip *put = addClip(a.proj, track, c);
    if (!put) return;

    a.sel.clear();
    a.sel.push_back(ClipRef{track, put->id});
    a.textClip = a.sel[0];
    a.changed();
    a.modal = MODAL_TEXT;
    a.say("caption added at %s", fmtTime(c.pos).c_str());
}

/* ------------------------------------------------------------------ *
 * The menu bar
 *
 * macOS only for now - see sn_appmenu.h. Drained once a frame rather than
 * acted on where AppKit delivers it, which is inside GLFW's event poll and
 * therefore inside a frame that is already drawing this project.
 * ------------------------------------------------------------------ */

static void appmenu(App &a)
{
    for (int cmd; (cmd = sn_appmenu_take()) != SN_CMD_NONE;) {
        /* A modal window has the screen. A menu command that reached past it
         * would act on a timeline nobody can currently see, which is what
         * keys() decided about the keyboard for the same reason. The pick is
         * still taken off the queue, so it is dropped rather than saved up to
         * fire the moment the dialog closes. */
        if (a.modal != MODAL_NONE) continue;

        switch (cmd) {
        case SN_CMD_NEW:           cmd_new(a); break;
        case SN_CMD_OPEN:          open_files(a, true); break;
        case SN_CMD_IMPORT:        open_files(a, false); break;
        case SN_CMD_SAVE:          save_project(a, false); break;
        case SN_CMD_SAVE_AS:       save_project(a, true); break;
        case SN_CMD_EXPORT:        cmd_export(a); break;

        case SN_CMD_UNDO:          cmd_undo(a); break;
        case SN_CMD_REDO:          cmd_redo(a); break;
        case SN_CMD_ADD_TEXT:      cmd_add_text(a); break;
        case SN_CMD_SPLIT:         doSplit(a); break;
        case SN_CMD_DELETE:        doDelete(a, false); break;
        case SN_CMD_RIPPLE_DELETE: doDelete(a, true); break;
        case SN_CMD_SELECT_ALL:    cmd_select_all(a); break;
        case SN_CMD_DESELECT:      a.clearSel(); break;

        default: break;
        }
    }
}

static void toolbar(App &a, Rectangle r)
{
    sn_ui &ui = a.ui;

    DrawRectangleRec(r, SN_PANEL);
    sn_divider(r.x, r.y + r.height - 1, r.width);

    float x = r.x + 8;
    const float y = r.y + 6;
    const float bw = 26, bh = 26;

    auto gap = [&]() {
        DrawRectangle((int)x + 3, (int)y + 3, 1, (int)bh - 6, SN_BORDER);
        x += 10;
    };

    if (sn_icon_button(&ui, 10, Rectangle{x, y, bw, bh}, SN_I_PLUS, 1, 0,
                       "add media to the bin (Ctrl+I)")) {
        open_files(a, false);
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 11, Rectangle{x, y, bw, bh}, SN_I_FOLDER, 1, 0,
                       "open a project (Ctrl+O)")) {
        open_files(a, true);
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 12, Rectangle{x, y, bw, bh}, SN_I_SAVE, 1, 0,
                       "save the project (Ctrl+S)")) {
        save_project(a, false);
    }
    x += bw + 4;
    gap();

    if (sn_icon_button(&ui, 13, Rectangle{x, y, bw, bh}, SN_I_UNDO, a.hist.canUndo(), 0,
                       "undo (Ctrl+Z)")) {
        cmd_undo(a);
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 14, Rectangle{x, y, bw, bh}, SN_I_REDO, a.hist.canRedo(), 0,
                       "redo (Ctrl+Shift+Z)")) {
        cmd_redo(a);
    }
    x += bw + 4;
    gap();

    if (sn_icon_button(&ui, 15, Rectangle{x, y, bw, bh}, SN_I_SPLIT, a.proj.duration() > 0, 0,
                       "cut every track at the playhead (S)")) {
        doSplit(a);
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 16, Rectangle{x, y, bw, bh}, SN_I_TRASH, !a.sel.empty(), 0,
                       "delete the selected clip (Del, or Shift+Del to close the gap)")) {
        doDelete(a, false);
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 25, Rectangle{x, y, bw, bh}, SN_I_TEXT, 1, 0,
                       "put a caption on the picture at the playhead (Ctrl+T)")) {
        cmd_add_text(a);
    }
    x += bw + 4;
    gap();

    if (sn_icon_button(&ui, 17, Rectangle{x, y, bw, bh}, SN_I_SNAP, 1, a.snapping,
                       a.snapping ? "snapping is on" : "snapping is off")) {
        a.snapping = !a.snapping;
        a.say("snapping %s", a.snapping ? "on" : "off");
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 18, Rectangle{x, y, bw, bh}, SN_I_ZOOM_OUT, 1, 0, "zoom out (-)")) {
        zoomTo(a, a.playhead, a.zoom * 0.7);
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 19, Rectangle{x, y, bw, bh}, SN_I_ZOOM_IN, 1, 0, "zoom in (+)")) {
        zoomTo(a, a.playhead, a.zoom * 1.4);
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 20, Rectangle{x, y, bw, bh}, SN_I_FIT, 1, 0,
                       "fit the whole timeline (F)")) {
        zoomFit(a);
    }
    x += bw + 4;
    gap();

    {
        char sz[64];
        snprintf(sz, sizeof sz, "%dx%d", a.proj.width, a.proj.height);
        Rectangle cb = {x, y, 86, bh};
        if (sn_button(&ui, 24, cb, sz, 1)) a.modal = MODAL_CANVAS;
        if (CheckCollisionPointRec(GetMousePosition(), cb) && !sn_ui_blocked(&ui))
            sn_tip(&ui, "the canvas: %dx%d at %.3f fps. Click to change it",
                   a.proj.width, a.proj.height, a.proj.fps);
        x += 90;
    }

    /* --- the right-hand end --- */
    Rectangle info = {r.x + r.width - 34, y, bw, bh};
    if (sn_icon_button(&ui, 21, info, SN_I_INFO, 1, a.modal == MODAL_INFO,
                       "what this is, and what it is built from")) {
        a.modal = a.modal == MODAL_INFO ? MODAL_NONE : MODAL_INFO;
    }

    Rectangle exp = {info.x - 92, y, 86, bh};
    if (sn_button_lit(&ui, 22, exp, "EXPORT", a.proj.duration() > 0)) cmd_export(a);

    /* --- the project's name ---
     *
     * Double-click it to change it. It is the name the export dialog suggests
     * a filename from, so this is also where you decide what the file is
     * going to be called - which is why it is here, next to the button that
     * writes it, rather than in a settings window.
     */
    if (a.renaming) {
        Rectangle nameR = {exp.x - 20 - 220, y + 2, 220, 22};
        if (sn_field(&ui, 23, nameR, a.renameText, "project name")) {
            /* nothing per keystroke - it is applied on Enter */
        }
        ui.focus = 23;

        const bool done = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
        const bool off = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                         !CheckCollisionPointRec(GetMousePosition(), nameR);

        if (done || off) {
            std::string n = a.renameText;
            while (!n.empty() && n.back() == ' ') n.pop_back();
            while (!n.empty() && n.front() == ' ') n.erase(n.begin());
            if (!n.empty() && n != a.proj.name) {
                a.proj.name = n;
                a.proj.dirty = true;
                exportRenamed(a);          /* the suggested filename follows */
                a.say("renamed to %s", n.c_str());
            }
            a.renaming = false;
            ui.focus = 0;
        }
        if (IsKeyPressed(KEY_ESCAPE)) { a.renaming = false; ui.focus = 0; }
    } else {
        char title[256];
        snprintf(title, sizeof title, "%s%s", a.proj.name.c_str(),
                 a.proj.dirty ? " *" : "");
        const float tw = sn_measure(&ui, SN_F_SMALL, title, 1.0f);
        Rectangle hit = {exp.x - 26 - tw, y + 2, tw + 12, 22};

        const bool hot = CheckCollisionPointRec(GetMousePosition(), hit) &&
                         !sn_ui_blocked(&ui);
        if (hot) {
            sn_cursor(&ui, MOUSE_CURSOR_IBEAM);
            sn_tip(&ui, "double-click to rename the project - the export is named after it");
            DrawRectangleLinesEx(hit, 1, SN_BORDER);
        }
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && sn_double_click(&ui, 23)) {
            a.renaming = true;
            a.renameText = a.proj.name;
            ui.caret = (int)a.renameText.size();
        }

        sn_text_spaced(&ui, SN_F_SMALL, title, hit.x + 6, y + 5,
                       a.proj.dirty ? SN_AMBER : SN_DIM);
    }
}

/* ------------------------------------------------------------------ *
 * Keyboard
 * ------------------------------------------------------------------ */

static void keys(App &a)
{
    /* While a text field has the keyboard, the shortcuts are off - otherwise
     * typing a filename with an s in it also cuts the clip. */
    if (a.ui.focus) return;

    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                      IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const double frame = a.proj.fps > 0 ? 1.0 / a.proj.fps : 1.0 / 30.0;

    if (a.modal != MODAL_NONE) return;

    if (ctrl) {
        if (IsKeyPressed(KEY_Z)) { if (shift) cmd_redo(a); else cmd_undo(a); }
        if (IsKeyPressed(KEY_Y)) cmd_redo(a);
        if (IsKeyPressed(KEY_S)) save_project(a, shift);
        if (IsKeyPressed(KEY_O)) open_files(a, true);
        if (IsKeyPressed(KEY_I)) open_files(a, false);
        if (IsKeyPressed(KEY_E)) cmd_export(a);
        if (IsKeyPressed(KEY_N)) cmd_new(a);
        if (IsKeyPressed(KEY_A)) cmd_select_all(a);
        if (IsKeyPressed(KEY_T)) cmd_add_text(a);
        return;
    }

    if (IsKeyPressed(KEY_SPACE)) {
        a.player.togglePlay();
        a.follow = true;
    }
    if (IsKeyPressed(KEY_S)) doSplit(a);
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) doDelete(a, shift);
    if (IsKeyPressed(KEY_F)) zoomFit(a);

    /* --- the canvas, from the keyboard ---
     *
     * R turns the selected caption a step at a time; C opens the crop and
     * layout window for the selected picture. Both are things the mouse can
     * already do on the preview, and both are here because the mouse is bad
     * at some of it: a fifteen degree step is not something a hand lands on,
     * and the handle that turns a caption can end up hard against the edge of
     * the pane when the caption is against the edge of the frame.
     *
     * Steps land on the fifteens rather than fifteen from wherever a drag
     * left it, so R four times from anything is always ninety degrees round
     * and always square. */
    if (IsKeyPressed(KEY_R)) {
        Clip *cap = nullptr;
        for (const ClipRef &cr : a.sel) {
            const Track *t = a.proj.track(cr.track);
            if (t && t->kind == TRACK_TEXT) { cap = a.proj.clip(cr); break; }
        }
        if (cap) {
            double n = std::round((cap->text.rotation + (shift ? -15.0 : 15.0)) / 15.0) *
                       15.0;
            while (n > 180.0) n -= 360.0;
            while (n <= -180.0) n += 360.0;
            cap->text.rotation = n;
            a.changed();
            a.say("turned to %.0f degrees", n);
        } else {
            a.say("select a caption first - click it on the preview");
        }
    }

    if (IsKeyPressed(KEY_C)) {
        if (a.proj.track(a.layoutTrack)) {
            a.modal = MODAL_LAYOUT;
        } else {
            a.say("select a picture first - click it on the preview");
        }
    }

    /* F12 is not handled here on purpose: raylib takes the screenshot itself,
     * into screenshot000.png beside the program, and a second handler on the
     * same key writes a second copy of the same picture. */
    if (IsKeyPressed(KEY_ESCAPE)) a.clearSel();

    if (IsKeyPressed(KEY_M)) {
        for (const ClipRef &r : a.sel)
            if (Clip *c = a.proj.clip(r)) c->muted = !c->muted;
        if (!a.sel.empty()) { a.changed(); a.say("muted"); }
    }

    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) zoomTo(a, a.playhead, a.zoom * 1.4);
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
        zoomTo(a, a.playhead, a.zoom * 0.7);

    double t = a.playhead;
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) t -= shift ? 1.0 : frame;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) t += shift ? 1.0 : frame;
    if (IsKeyPressed(KEY_HOME)) t = 0;
    if (IsKeyPressed(KEY_END)) t = a.proj.duration();
    if (IsKeyPressed(KEY_COMMA)) t = prevEdit(a.proj, a.playhead);
    if (IsKeyPressed(KEY_PERIOD)) t = nextEdit(a.proj, a.playhead);

    if (t != a.playhead) {
        a.playhead = std::max(0.0, t);
        a.player.seek(a.playhead);
        a.follow = true;
    }
}

/* ------------------------------------------------------------------ *
 * Menus opened by the panes
 * ------------------------------------------------------------------ */

static void menus(App &a)
{
    int tag = 0;
    const int pick = sn_menu_take(&a.ui, &tag);
    if (pick < 0) return;

    if (tag == 100) {                     /* a clip on the timeline */
        if (a.sel.empty()) return;
        const ClipRef r = a.sel[0];
        Clip *c = a.proj.clip(r);
        if (!c) return;

        /* What the row means rather than which row it was: the menu's rows
         * come and go with what is under the pointer. See App::clipMenu. */
        const int what = pick >= 0 && pick < (int)a.clipMenu.size()
                             ? a.clipMenu[(size_t)pick]
                             : (int)App::CLIP_NOTHING;

        switch (what) {
        case App::CLIP_SPLIT: {
            /* Split at the playhead if it is inside this clip, otherwise in
             * the middle of it - which is what "split here" means when the
             * playhead is somewhere else entirely. */
            const double at = c->covers(a.playhead) ? a.playhead : c->pos + c->dur() * 0.5;
            splitAt(a.proj, at, &r);
            a.changed();
            a.say("cut at %s", fmtTime(at).c_str());
            break;
        }
        case App::CLIP_DELETE: doDelete(a, false); break;
        case App::CLIP_RIPPLE: doDelete(a, true); break;
        case App::CLIP_MUTE: c->muted = !c->muted; a.changed(); break;
        case App::CLIP_CLEAR_FX: {
            /* Fades are on the track's lane now, so this clears the lane
             * rather than two numbers on the clip. It is still offered from a
             * clip's menu because that is where somebody looking to undo a
             * fade will right-click. */
            Track *t = a.proj.track(r.track);
            if (t && !t->fx.empty()) {
                const int n = (int)t->fx.size();
                t->fx.clear();
                a.changed();
                a.say("cleared %d effect%s from %s", n, n == 1 ? "" : "s",
                      t->name.c_str());
            }
            break;
        }
        case App::CLIP_SPLIT_CHANNELS: {
            const int n = splitChannels(a.proj, r);
            if (n > 0) {
                a.clearSel();
                a.changed();
                a.say("split into %d mono clips, one per channel", n);
            } else {
                a.complain("nothing to split - that clip is already one channel");
            }
            break;
        }
        case App::CLIP_UNLINK: {
            /* Unlink. Every clip that shares the link id loses it, which is
             * both halves and no more: a split earlier gave each pair its own
             * id precisely so this cannot reach across a cut. */
            const int link = c->link;
            if (!link) break;
            int n = 0;
            for (Track &t : a.proj.tracks)
                for (Clip &x : t.clips)
                    if (x.link == link) { x.link = 0; n++; }
            a.clearSel();
            a.changed();
            a.say("unlinked - the %d halves move on their own now", n);
            break;
        }
        default: break;
        }
        return;
    }

    if (tag == 102) {                     /* the effects lane */
        Track *t = a.proj.track(a.fxSel.track);
        if (!t) return;

        const int what = pick >= 0 && pick < (int)a.fxMenu.size()
                             ? a.fxMenu[(size_t)pick]
                             : (int)App::FX_M_NOTHING;

        const double from = a.fxRangeFrom, to = a.fxRangeTo;
        const bool haveRange = to > from;

        switch (what) {
        case App::FX_M_IN:
            if (haveRange) fxPreset(*t, FX_FADE_IN, from, to);
            break;
        case App::FX_M_OUT:
            if (haveRange) fxPreset(*t, FX_FADE_OUT, from, to);
            break;
        case App::FX_M_INOUT:
            if (haveRange) fxPreset(*t, FX_IN_OUT, from, to);
            break;
        case App::FX_M_PULSE:
            if (haveRange) fxPreset(*t, FX_PULSE, from, to);
            break;
        case App::FX_M_WAVE:
            if (haveRange) fxPreset(*t, FX_WAVE, from, to);
            break;

        case App::FX_M_HOLD:
            if (a.fxSel.index >= 0 && a.fxSel.index < (int)t->fx.size()) {
                FxPoint &pt = t->fx[(size_t)a.fxSel.index];
                pt.hold = !pt.hold;
                a.say(pt.hold ? "that point holds until the next one"
                              : "that point slides to the next one");
            }
            break;

        case App::FX_M_DELETE:
            if (a.fxSel.index >= 0 && a.fxSel.index < (int)t->fx.size()) {
                t->fx.erase(t->fx.begin() + a.fxSel.index);
                a.fxSel = App::FxRef{};
            }
            break;

        case App::FX_M_CLEAR: {
            const int n = (int)t->fx.size();
            t->fx.clear();
            a.fxSel = App::FxRef{};
            a.say("cleared %d point%s from %s", n, n == 1 ? "" : "s", t->name.c_str());
            break;
        }

        default: return;
        }

        a.changed();
        return;
    }

    if (tag == 101) {                     /* a caption on the timeline */
        switch (pick) {
        case 0: a.modal = MODAL_TEXT; break;
        case 2: {
            if (!a.sel.empty()) {
                const ClipRef r = a.sel[0];
                const Clip *c = a.proj.clip(r);
                if (c) {
                    const double at = c->covers(a.playhead) ? a.playhead
                                                            : c->pos + c->dur() * 0.5;
                    splitAt(a.proj, at, &r);
                    a.changed();
                    a.say("cut at %s", fmtTime(at).c_str());
                }
            }
            break;
        }
        case 3: doDelete(a, false); break;
        case 4: doDelete(a, true); break;
        default: break;
        }
        return;
    }

    if (tag > 200) {                      /* a bin item */
        const int id = tag - 200;
        const BinItem *b = a.proj.item(id);
        if (!b) return;

        switch (pick) {
        case 0:
            a.playhead = placeItem(a.proj, id, a.playhead);
            a.changed();
            a.player.seek(a.playhead);
            break;
        case 1: {
            char buf[1024] = {0};
            if (sn_open_dialog(GetWindowHandle(), "Relink", ".", "Media", MEDIA_EXTS, 0, buf,
                               sizeof buf) == SN_DLG_OK) {
                std::string err;
                if (relink(a.proj, id, buf, &err)) {
                    a.thumbs.erase(id);
                    a.changed();
                    a.say("relinked");
                } else {
                    a.complain("%s", err.c_str());
                }
            }
            break;
        }
        case 3: {
            /* Taking a file out of the bin takes its clips with it -
             * otherwise the timeline holds clips of a source that is not
             * there, and every one of them is a silent black gap. */
            for (Track &t : a.proj.tracks)
                t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                             [&](const Clip &c) { return c.source == id; }),
                              t.clips.end());
            for (size_t i = 0; i < a.proj.bin.size(); i++)
                if (a.proj.bin[i].id == id) { a.proj.bin.erase(a.proj.bin.begin() + i); break; }

            auto it = a.thumbs.find(id);
            if (it != a.thumbs.end()) {
                if (it->second.tex.id) UnloadTexture(it->second.tex);
                a.thumbs.erase(it);
            }
            a.sel.clear();
            a.changed();
            a.say("removed %s", b->info.name.c_str());
            break;
        }
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

/* Draw a few frames and write a png. Not a feature - a way to see what the
 * window looks like from a machine with no screen, which is how the layout
 * gets checked in CI and how it got checked while it was being written. */
static const char *g_shot = nullptr;
static int g_shotAfter = 60;


/* ------------------------------------------------------------------ *
 * How long each part of starting up took
 *
 * Kept because startup is the one thing that cannot be measured from here:
 * it is different on every machine, and the interesting cases - a driver
 * negotiating a pixel format, a sound stack enumerating devices, a virus
 * scanner reading a 40 MB executable - are all somewhere this program cannot
 * see. What it can do is write down when it reached each line, so the answer
 * is a list of numbers rather than a guess.
 *
 * `--timing` prints it. The clock starts at the first instruction of main,
 * so the first entry also says how much went on before any of this ran.
 * ------------------------------------------------------------------ */

namespace {

using sn_clock = std::chrono::steady_clock;
sn_clock::time_point g_t0;
bool g_timing = false;

/* Set by --msaa, which is off by default. See where it is used. */
bool g_msaa = false;

/* Everything --timing prints also goes to a file, because the machine with a
 * startup problem on it is rarely the machine with a terminal in front of it,
 * and a file can be attached to a message. Written to the temporary
 * directory rather than beside the executable: this program installs into
 * Program Files, which is not writable by the person running it. */
FILE *g_logf = nullptr;
std::string g_logPath;

void log_open()
{
    if (g_logf || g_logPath.empty()) return;
    g_logf = fopen(g_logPath.c_str(), "w");
}

/* One line, to the console and to the log. */
void emit(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (g_timing) { fputs(buf, stdout); fflush(stdout); }
    if (g_logf) { fputs(buf, g_logf); fflush(g_logf); }
}

/* Filled in by trace_hook from raylib's own startup log. */
char g_glVendor[128];
char g_glRenderer[128];

struct Mark {
    const char *what;
    double at;                 /* ms since main started */
};
Mark g_marks[16];
int g_nmarks = 0;

void mark(const char *what)
{
    if (g_nmarks >= (int)(sizeof g_marks / sizeof g_marks[0])) return;
    const double at =
        std::chrono::duration<double, std::milli>(sn_clock::now() - g_t0).count();
    g_marks[g_nmarks++] = Mark{what, at};

    /* Printed as it happens rather than only in the summary at the end, and
     * flushed. Startup is exactly the part of the program that can fail by
     * not finishing - a graphics driver that never returns a context, a sound
     * stack waiting on a device that is not there - and a report printed
     * afterwards says nothing at all about the run that hung. The last line
     * on screen is the phase that did not come back. */
    emit("  %8.1f ms  %s\n", at, what);
}

} /* namespace */

/* For the info window, which is where these numbers can be read on a machine
 * with no console to print to - which is every Windows machine, since the
 * executable is linked for the windowing subsystem and its standard output
 * goes nowhere. */
int startupPhases() { return g_nmarks; }
const char *startupPhaseName(int i) { return i >= 0 && i < g_nmarks ? g_marks[i].what : ""; }
double startupPhaseMs(int i) { return i >= 0 && i < g_nmarks ? g_marks[i].at : 0.0; }

/* When something was first on screen, and when the whole of startup was done.
 * The gap between them is the part this program controls. */
double startupWindowMs()
{
    for (int i = 0; i < g_nmarks; i++)
        if (std::strcmp(g_marks[i].what, "first frame") == 0) return g_marks[i].at;
    return 0.0;
}

double startupReadyMs() { return g_nmarks ? g_marks[g_nmarks - 1].at : 0.0; }

/* Who is actually drawing. A machine that turns out to be running a software
 * renderer explains a slow start and a slow everything-else at once, and it
 * is not something anybody can be expected to know off hand. */
const char *glRenderer() { return g_glRenderer[0] ? g_glRenderer : "unknown"; }
const char *glVendor() { return g_glVendor[0] ? g_glVendor : "unknown"; }

namespace {

/* What raylib says about itself, stamped with when it said it.
 *
 * InitWindow is one call from here and ten seconds long on some Windows
 * machines, so the only way to see inside it is to timestamp the log it
 * writes as it goes. The step before the gap is the step that took the time.
 *
 * The callback is installed whether or not anyone asked for timings, because
 * it is also where the name of the graphics driver comes past - and a driver
 * that turns out to be a software renderer explains a great deal, both about
 * a slow start and about everything after it.
 */
void trace_hook(int level, const char *text, va_list args)
{
    char buf[512];
    vsnprintf(buf, sizeof buf, text, args);

    /* raylib prints these as their own lines under "OpenGL device
     * information", one field each. */
    const char *v = strstr(buf, "Vendor:");
    const char *r = strstr(buf, "Renderer:");
    if (v && !g_glVendor[0]) snprintf(g_glVendor, sizeof g_glVendor, "%s", v + 8);
    if (r && !g_glRenderer[0]) snprintf(g_glRenderer, sizeof g_glRenderer, "%s", r + 10);

    /* Quiet unless asked. Warnings and worse always come through, which is
     * what the log level used to be set to. */
    if (!g_timing && !g_logf && level < LOG_WARNING) return;

    const double at =
        std::chrono::duration<double, std::milli>(sn_clock::now() - g_t0).count();
    emit("  %8.1f ms  raylib: %s\n", at, buf);
}

void timing_report()
{
    if (!g_timing && !g_logf) return;

    emit("\nstartup, milliseconds from the first line of main:\n");
    double prev = 0;
    for (int i = 0; i < g_nmarks; i++) {
        emit("  %8.1f  %+7.1f  %s\n", g_marks[i].at, g_marks[i].at - prev,
             g_marks[i].what);
        prev = g_marks[i].at;
    }
    emit("\n  the first number is also how long the window took to appear.\n"
         "  anything before that - the loader, a scanner reading the whole\n"
         "  executable, a driver picking a pixel format - is not in this list\n"
         "  and is not something the program can shorten from the inside.\n\n");

    if (g_logf && g_timing) printf("  this was also written to %s\n\n", g_logPath.c_str());
}

/* One cleared frame, before any of the work below.
 *
 * There used to be a splash screen here: a panel with the wordmark, the
 * version and a progress bar, redrawn between the phases of startup. It was
 * built when startup could take ten and a half seconds, and it existed to say
 * that the ten and a half seconds were being spent rather than lost. That
 * cause is gone - it was GLFW enumerating game controllers, and it is not
 * enumerated any more - so what is left is furniture in front of a program
 * that is already ready, which is the thing this one's README makes fun of
 * Clipchamp for.
 *
 * This much stays. Between the window appearing and the first pass of the
 * loop the back buffer holds whatever was there before, and on some systems
 * that is the desktop behind it; one clear means the first thing on screen is
 * this program's own background. It is also what "first frame" is a mark for,
 * which is the number the information window reports as how long the window
 * took to appear. */
void first_frame()
{
    BeginDrawing();
    ClearBackground(SN_BG);
    EndDrawing();
}

} /* namespace */

static int run(int argc, char **argv)
{
    /* No multisampling. It is asked for with --msaa, and there is a good
     * reason nobody should bother.
     *
     * Asking for a 4x multisampled pixel format cost ten and a half seconds
     * of startup on a Windows machine with an RTX 4080 and a current driver -
     * measured, with a clock on raylib's own log: 29.7 ms at "Trying to
     * enable MSAA x4", 10495.4 ms at the next line. The whole of that
     * program's slow start was this one hint. It is not a broken machine
     * either; the same driver hands over an ordinary pixel format at once.
     *
     * And what it bought: 703 pixels out of 972,800 - seven hundredths of one
     * per cent of the window - because the only curved thing on screen is
     * S. Tarr's star and the rest is rectangles and text. Both versions were
     * rendered and compared. At four times magnification the two stars are
     * hard to tell apart.
     *
     * Ten seconds for that is not a trade, so the hint is gone. The switch
     * stays for anyone who wants it and knows what it may cost them. */
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT;
    if (g_msaa) flags |= FLAG_MSAA_4X_HINT;
    SetConfigFlags(flags);
    /* Everything reaches the hook, which decides what to print. It has to see
     * the informational lines even when nothing is being timed, because the
     * graphics driver's name comes past as one of them. */
    SetTraceLogLevel(LOG_ALL);
    SetTraceLogCallback(trace_hook);
    mark("before window");
    emit("  ... asking for the window and a GL context\n");
    InitWindow(1280, 760, SN_NAME " " SN_VERSION);
    mark("window + GL");
    SetWindowMinSize(900, 560);

    /* File and Edit, on the platforms that have a bar to put them in. After
     * InitWindow because on macOS that is what creates the NSApplication and
     * the menu bar this inserts into; before it there is nothing there. */
    sn_appmenu_install();
    SetExitKey(KEY_NULL);            /* Escape clears the selection */

    /* The font before anything else. Everything this program draws is drawn
     * with it, and it costs a millisecond and a half. */
    App a;
    sn_ui_init(&a.ui);
    mark("font");
    first_frame();
    mark("first frame");

    emit("  ... opening an audio device\n");
    InitAudioDevice();
    mark("audio");

    {
        /* Four sizes rather than one: a window manager picks the nearest and
         * scales it, and the 16 pixel version is not the big one made smaller
         * - it has the film strip taken out so the camera has room to read.
         * Handing over only the 64 would throw that away. */
        struct { const unsigned char *png; unsigned len; } src[] = {
            {SN_ICON_16, SN_ICON_16_LEN}, {SN_ICON_32, SN_ICON_32_LEN},
            {SN_ICON_48, SN_ICON_48_LEN}, {SN_ICON_64, SN_ICON_64_LEN},
        };
        const int n = (int)(sizeof src / sizeof src[0]);

        Image icons[4];
        int got = 0;
        for (int i = 0; i < n; i++) {
            icons[got] = LoadImageFromMemory(".png", src[i].png, (int)src[i].len);
            if (icons[got].data == nullptr) continue;
            /* SetWindowIcons wants RGBA and says nothing when given anything
             * else; the loader gives that for these files, but a re-export
             * that drops the alpha channel would otherwise show up as a
             * missing icon rather than as a fixable mistake. */
            ImageFormat(&icons[got], PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            got++;
        }
        if (got) SetWindowIcons(icons, got);
        for (int i = 0; i < got; i++) UnloadImage(icons[i]);
    }
    mark("icons");

    a.proj = newProject();
    a.hist.reset(a.proj);
    a.player.start();
    a.player.setProject(a.proj, a.rev);

    /* Anything on the command line is imported, and a project file among it
     * is opened - the same rules as a drop, so `bencsnip *.mp4` works. */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            /* Skip the option's argument too, or a filename is imported that
             * was never meant as one. */
            if (!strcmp(argv[i], "--shot") || !strcmp(argv[i], "--frames") ||
                !strcmp(argv[i], "--log"))
                i++;
            continue;
        }
        doImport(a, argv[i], true);
    }
    /* The playhead ends up past the last import; the point of opening a file
     * is to look at its beginning. */
    a.playhead = 0;
    a.player.seek(0);

    /* Fitting needs a timeline that has been laid out, and nothing has been
     * laid out until the first frame. */
    bool fitWanted = a.proj.duration() > 0;

    a.hist.reset(a.proj);
    a.proj.dirty = false;

    mark("files named");

    timing_report();

    int frames = 0;

    while (!a.quit) {
        if (g_shot && ++frames > g_shotAfter) {
            TakeScreenshot(g_shot);
            break;
        }
        if (WindowShouldClose()) {
            if (a.proj.dirty) {
                a.confirmText = "There are unsaved changes. Close anyway?";
                a.confirmTag = 1;
                a.modal = MODAL_CONFIRM;
            } else {
                break;
            }
        }

        sn_ui_frame(&a.ui);

        /* --- files dropped on the window --- */
        if (IsFileDropped()) {
            FilePathList drop = LoadDroppedFiles();
            for (unsigned i = 0; i < drop.count; i++) doImport(a, drop.paths[i], false);
            if (drop.count)
                a.say("added %u file%s to the bin", drop.count, drop.count == 1 ? "" : "s");
            UnloadDroppedFiles(drop);
        }

        /* --- layout --- */
        const float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
        Rectangle rTool = {0, 0, W, TOOLBAR_H};
        Rectangle rStatus = {0, H - STATUS_H, W, STATUS_H};

        /* The timeline gets a third of the window, between a floor that fits
         * two tracks and a ceiling that leaves the preview usable. */
        const float tlH = std::max(200.0f, std::min(H * 0.42f, H - 340.0f));
        Rectangle rTimeline = {0, rStatus.y - tlH, W, tlH};
        Rectangle rBin = {0, rTool.height, BIN_W, rTimeline.y - rTool.height};
        Rectangle rPreview = {BIN_W, rTool.height, W - BIN_W, rTimeline.y - rTool.height};

        a.rBin = rBin;
        a.rStatus = rStatus;
        a.rTimeline = rTimeline;

        if (fitWanted) { zoomFit(a); fitWanted = false; }

        /* --- the player, and following it --- */
        a.player.setProject(a.proj, a.rev);
        a.player.setEnd(a.proj.duration());

        if (a.player.playing()) {
            a.playhead = a.player.position();
            if (a.follow) {
                const double left = a.scroll;
                const double right = a.scroll + (rTimeline.width - 104) / a.zoom;
                if (a.playhead < left || a.playhead > right - 0.5 / a.zoom)
                    a.scroll = std::max(0.0, a.playhead - (right - left) * 0.15);
            }
        }

        /* A modal takes the mouse away from everything under it. */
        const bool modal = a.modal != MODAL_NONE;

        BeginDrawing();
        ClearBackground(SN_BG);

        a.ui.suppress = modal;

        toolbar(a, rTool);
        binPane(a, rBin);
        previewPane(a, rPreview);
        timelinePane(a, rTimeline);

        /* --- a bin drag that let go somewhere else --- */
        if (a.drag == DRAG_FROM_BIN) {
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                a.drag = DRAG_NONE;
                a.dragBin = 0;
            } else {
                /* Carried under the pointer, so it is obvious something is
                 * being dragged and what. */
                const BinItem *b = a.proj.item(a.dragBin);
                const Vector2 m = GetMousePosition();
                if (b && !CheckCollisionPointRec(m, rTimeline)) {
                    Rectangle g = {m.x + 10, m.y - 8, 150, 18};
                    sn_panel(g, SN_PANEL, SN_ACCENT);
                    sn_text_clip(&a.ui, SN_F_TINY, b->info.name.c_str(), g.x + 5, g.y + 3,
                                 g.width - 10, SN_TEXT);
                }
            }
        }

        /* --- status --- */
        DrawRectangleRec(rStatus, SN_PANEL);
        sn_divider(rStatus.x, rStatus.y, rStatus.width);
        {
            const bool fresh = GetTime() - a.statusAt < 6.0;
            const char *msg = fresh && !a.status.empty() ? a.status.c_str()
                              : a.ui.tip[0]              ? a.ui.tip
                                                         : "";
            sn_text(&a.ui, SN_F_TINY, msg, rStatus.x + 8, rStatus.y + 5,
                    fresh && a.statusBad ? SN_ALERT : (fresh ? SN_TEXT : SN_DIM));

            char right[128];
            snprintf(right, sizeof right, "%dx%d  %.2f fps  %d clip%s", a.proj.width,
                     a.proj.height, a.proj.fps, [&] {
                         int n = 0;
                         for (const Track &t : a.proj.tracks) n += (int)t.clips.size();
                         return n;
                     }(),
                     [&] {
                         int n = 0;
                         for (const Track &t : a.proj.tracks) n += (int)t.clips.size();
                         return n == 1 ? "" : "s";
                     }());
            const float rw = sn_measure(&a.ui, SN_F_TINY, right, 0.0f);
            sn_text(&a.ui, SN_F_TINY, right, rStatus.x + rStatus.width - rw - 8,
                    rStatus.y + 5, SN_EDGE);
        }

        /* --- modals --- */
        a.ui.suppress = false;
        switch (a.modal) {
        case MODAL_EXPORT: exportDialog(a); break;
        case MODAL_LAYOUT: layoutDialog(a); break;
        case MODAL_TEXT:   textDialog(a); break;
        case MODAL_CANVAS: canvasDialog(a); break;
        case MODAL_INFO: infoWindow(a); break;
        case MODAL_CONFIRM: confirmDialog(a); break;
        case MODAL_OPEN:
        case MODAL_LOAD:
        case MODAL_SAVE: {
            std::string path;
            const bool save = a.modal == MODAL_SAVE;
            const int rc = fileDialog(a, Rectangle{0, 0, 0, 0},
                                      save ? "SAVE PROJECT"
                                           : (a.modal == MODAL_LOAD ? "OPEN PROJECT"
                                                                    : "ADD MEDIA"),
                                      save, &path);
            if (rc == 1) {
                if (save) {
                    if (path.find(".bencsnip") == std::string::npos) path += ".bencsnip";
                    std::string err;
                    if (saveProject(a.proj, path, &err)) {
                        a.proj.path = path;
                        a.proj.name = GetFileName(path.c_str());
                        a.proj.dirty = false;
                        a.say("saved %s", a.proj.name.c_str());
                    } else a.complain("%s", err.c_str());
                } else {
                    doImport(a, path, false);
                }
                a.modal = MODAL_NONE;
            } else if (rc == -1) {
                a.modal = MODAL_NONE;
            }
            break;
        }
        default: break;
        }

        menus(a);
        sn_ui_overlay(&a.ui);

        /* Once, at the end, after every pane has had its say. */
        sn_cursor_apply(&a.ui);

        EndDrawing();

        keys(a);

        /* After EndDrawing, beside keys(), because that is where the events
         * this drains have just arrived: EndDrawing polls, AppKit dispatches
         * the menu action during the poll, and acting on it here means the
         * project changes between frames rather than under one. */
        appmenu(a);
    }

    /* --- shutdown --- */
    if (a.exStatus.running.load()) a.exStatus.cancel.store(true);
    if (a.exThread && a.exThread->joinable()) a.exThread->join();

    a.player.stop();
    binShutdown(a);
    peaksShutdown();
    if (a.preview.id) UnloadTexture(a.preview);
    sn_ui_free(&a.ui);
    /* Give the pointer back before the window goes. Quitting while the loop
     * glyph has it hidden is the one way it can be left hidden for good,
     * since after this there is nobody to put it back. */
    ShowCursor();
    SetMouseCursor(MOUSE_CURSOR_DEFAULT);

    CloseAudioDevice();
    CloseWindow();
    return 0;
}

} /* namespace sn */

/* The probe in tools/glprobe.c, compiled into this executable as well as
 * shipped beside it. Same calls, same order, different process - which is the
 * only way to tell "the sequence is slow" apart from "this binary is slow". */
extern "C" int sn_glprobe_run(void);

int main(int argc, char **argv)
{
    sn::g_t0 = sn::sn_clock::now();

    /* Windows links this for the windowing subsystem, so that a double-click
     * does not open a console behind the window - and the cost is that printf
     * goes nowhere, including for --version, --help and --timing, which exist
     * to be read from a terminal. This borrows the console of whatever
     * started us, and does nothing at all anywhere else. */
    sn_attach_console();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--timing")) sn::g_timing = true;
        if (!strcmp(argv[i], "--msaa")) sn::g_msaa = true;
        if (!strcmp(argv[i], "--log") && i + 1 < argc) sn::g_logPath = argv[i + 1];
    }

    /* A log is written whenever timings were asked for, and --log puts it
     * somewhere chosen. Somewhere writable by default: this installs into
     * Program Files, and the person running it cannot write there. */
    if (sn::g_timing && sn::g_logPath.empty()) {
        const char *tmp = getenv("TEMP");
        if (!tmp || !*tmp) tmp = getenv("TMPDIR");
        if (!tmp || !*tmp) tmp = "/tmp";
        sn::g_logPath = std::string(tmp) + "/bencsnip-startup.log";
    }
    if (!sn::g_logPath.empty()) { sn::g_timing = true; sn::log_open(); }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("%s %s\n", SN_NAME, SN_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            sn::g_shot = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            sn::g_shotAfter = atoi(argv[++i]);
            continue;
        }
        if (!strcmp(argv[i], "--glprobe")) {
            /* Before any window: the point is to be the first thing in this
             * process to touch the graphics driver and the shell. */
            sn_attach_console();
            return sn_glprobe_run();
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("%s %s - a video editor\n\n", SN_NAME, SN_VERSION);
            printf("  bencsnip [FILE ...]\n\n");
            printf("Files on the command line are imported and laid on the timeline.\n");
            printf("A .bencsnip project file among them is opened instead.\n\n");
            printf("  --shot FILE.png    draw a few frames, write a screenshot, exit\n");
            printf("  --frames N         how many frames to draw first (default 60)\n");
            printf("  --timing           print how long each part of starting up took\n");
            printf("  --glprobe          time every Windows call on the way to a GL\n");
            printf("                     context, and stop. For a slow-opening window\n");
            printf("  --log FILE         write the startup log here. Implied by --timing,\n");
            printf("                     which writes one to the temporary directory\n");
            printf("  --msaa             ask for a multisampled pixel format. Smooths\n");
            printf("                     the one curved thing on screen, and on some\n");
            printf("                     drivers costs ten seconds of startup for it\n");
            return 0;
        }
    }
    return sn::run(argc, argv);
}
