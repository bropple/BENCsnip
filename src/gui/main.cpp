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
 * better than any list. */
static const char *MEDIA_EXTS =
    "mp4 mov mkv webm avi m4v mpg mpeg wmv flv ts m2ts mts 3gp ogv "
    "mp3 wav flac m4a aac ogg opus wma aiff aif";

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
        if (loadProject(&p, path, &err)) {
            a.proj = p;
            a.hist.reset(a.proj);
            a.thumbs.clear();
            a.playhead = 0;
            a.sel.clear();
            a.changed(true);
            zoomFit(a);
            a.say("opened %s", GetFileName(path.c_str()));
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

        const bool overView = CheckCollisionPointRec(m, view) && !sn_ui_blocked(&a.ui);
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
        if (a.hist.undo(&a.proj)) { a.rev++; a.sel.clear(); a.say("undone"); }
    }
    x += bw + 4;
    if (sn_icon_button(&ui, 14, Rectangle{x, y, bw, bh}, SN_I_REDO, a.hist.canRedo(), 0,
                       "redo (Ctrl+Shift+Z)")) {
        if (a.hist.redo(&a.proj)) { a.rev++; a.sel.clear(); a.say("redone"); }
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
    if (sn_button_lit(&ui, 22, exp, "EXPORT", a.proj.duration() > 0)) {
        if (a.proj.duration() > 0) {
            exportDialogPrepare(a);
            a.modal = MODAL_EXPORT;
        }
    }

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
        if (IsKeyPressed(KEY_Z)) {
            if (shift ? a.hist.redo(&a.proj) : a.hist.undo(&a.proj)) {
                a.rev++;
                a.sel.clear();
                a.player.setEnd(a.proj.duration());
                a.say(shift ? "redone" : "undone");
            }
        }
        if (IsKeyPressed(KEY_Y) && a.hist.redo(&a.proj)) {
            a.rev++;
            a.sel.clear();
            a.say("redone");
        }
        if (IsKeyPressed(KEY_S)) save_project(a, shift);
        if (IsKeyPressed(KEY_O)) open_files(a, true);
        if (IsKeyPressed(KEY_I)) open_files(a, false);
        if (IsKeyPressed(KEY_E) && a.proj.duration() > 0) {
            exportDialogPrepare(a);
            a.modal = MODAL_EXPORT;
        }
        if (IsKeyPressed(KEY_N)) {
            if (a.proj.dirty) {
                a.confirmText = "Throw away the unsaved changes?";
                a.confirmTag = 2;
                a.modal = MODAL_CONFIRM;
            } else {
                a.proj = newProject();
                a.hist.reset(a.proj);
                a.thumbs.clear();
                a.playhead = 0;
                a.changed(true);
            }
        }
        if (IsKeyPressed(KEY_A)) {
            a.sel.clear();
            for (size_t i = 0; i < a.proj.tracks.size(); i++)
                for (const Clip &c : a.proj.tracks[i].clips)
                    a.sel.push_back(ClipRef{(int)i, c.id});
            a.say("selected %d clips", (int)a.sel.size());
        }
        return;
    }

    if (IsKeyPressed(KEY_SPACE)) {
        a.player.togglePlay();
        a.follow = true;
    }
    if (IsKeyPressed(KEY_S)) doSplit(a);
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) doDelete(a, shift);
    if (IsKeyPressed(KEY_F)) zoomFit(a);

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

        switch (pick) {
        case 0: {
            /* Split at the playhead if it is inside this clip, otherwise in
             * the middle of it - which is what "split here" means when the
             * playhead is somewhere else entirely. */
            const double at = c->covers(a.playhead) ? a.playhead : c->pos + c->dur() * 0.5;
            splitAt(a.proj, at, &r);
            a.changed();
            a.say("cut at %s", fmtTime(at).c_str());
            break;
        }
        case 1: doDelete(a, false); break;
        case 2: doDelete(a, true); break;
        case 4: c->muted = !c->muted; a.changed(); break;
        case 5: c->fadeIn = c->fadeOut = 0; a.changed(); a.say("fades cleared"); break;
        case 6: {
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
 * The splash
 *
 * Not decoration, and not a delay: the window exists from the moment
 * InitWindow returns, and without this it sits there empty until every other
 * piece of startup has finished. On a machine where the graphics driver takes
 * its time, or where a project on the command line has twenty clips to probe,
 * that empty window is the program looking hung while it works.
 *
 * So it draws once per phase, saying which phase. Everything it needs - the
 * icon, the wordmark, the font - is already inside the executable, and the
 * font is loaded first precisely so this can use it.
 *
 * What it cannot cover is anything before InitWindow: the loader mapping a
 * 28 MB binary, a virus scanner reading all of it, Gatekeeper verifying a
 * signature. Those happen before a line of this program runs, and no splash
 * screen in any program has ever covered them.
 * ------------------------------------------------------------------ */

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
    if (g_timing) {
        printf("  %8.1f ms  %s\n", at, what);
        fflush(stdout);
    }
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

namespace {

void timing_report()
{
    if (!g_timing) return;
    printf("\nstartup, milliseconds from the first line of main:\n");
    double prev = 0;
    for (int i = 0; i < g_nmarks; i++) {
        printf("  %8.1f  %+7.1f  %s\n", g_marks[i].at, g_marks[i].at - prev,
               g_marks[i].what);
        prev = g_marks[i].at;
    }
    printf("\n  the first number is also how long the window took to appear.\n"
           "  anything before that - the loader, a scanner reading the whole\n"
           "  executable, a driver picking a pixel format - is not in this list\n"
           "  and is not something the program can shorten from the inside.\n\n");
}

struct Splash {
    Texture2D mark = {};      /* the BENCO wordmark   */
    Texture2D icon = {};      /* the program's icon   */
    bool ready = false;

    /* No minimum time on screen, deliberately. It is up for exactly as long
     * as there is work behind it, which on a fast machine is about a tenth of
     * a second - and a splash screen that outstays the loading it reports is
     * the thing this program's README makes fun of Clipchamp for. */
};

Splash g_splash;

void splashInit(App &a)
{
    Image m = LoadImageFromMemory(".png", SN_LOGO_PNG, (int)SN_LOGO_PNG_LEN);
    if (m.data) { g_splash.mark = LoadTextureFromImage(m); UnloadImage(m); }

    Image i = LoadImageFromMemory(".png", SN_ICON_64, (int)SN_ICON_64_LEN);
    if (i.data) { g_splash.icon = LoadTextureFromImage(i); UnloadImage(i); }

    g_splash.ready = true;
    (void)a;
}

/* One frame. `frac` is how far through startup we are, and is honest rather
 * than smooth - it moves when something actually finished. */
void splashDraw(App &a, const char *phase, float frac)
{
    if (!g_splash.ready) return;

    const float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();

    BeginDrawing();
    ClearBackground(SN_BG);

    /* A panel rather than the whole window: the window is 1280 wide and a
     * wordmark stranded in the middle of that much near-black reads as a
     * program that has failed to draw itself. */
    Rectangle box = {std::floor(W * 0.5f - 210), std::floor(H * 0.5f - 100), 420, 200};
    sn_panel(box, SN_PANEL, SN_BORDER);

    if (g_splash.icon.id) {
        DrawTexturePro(g_splash.icon,
                       Rectangle{0, 0, (float)g_splash.icon.width,
                                 (float)g_splash.icon.height},
                       Rectangle{box.x + 26, box.y + 30, 64, 64}, Vector2{0, 0}, 0,
                       WHITE);
    }

    if (g_splash.mark.id) {
        const float mw = 150.0f;
        const float mh = mw * (float)g_splash.mark.height / g_splash.mark.width;
        DrawTexturePro(g_splash.mark,
                       Rectangle{0, 0, (float)g_splash.mark.width,
                                 (float)g_splash.mark.height},
                       Rectangle{box.x + 110, box.y + 30, mw, mh}, Vector2{0, 0}, 0,
                       SN_EDGE);
    }

    sn_text_spaced(&a.ui, SN_F_TITLE, SN_NAME, box.x + 110, box.y + 62, SN_TEXT);
    sn_text(&a.ui, SN_F_SMALL, "version " SN_VERSION, box.x + 112, box.y + 96, SN_DIM);

    sn_divider(box.x + 26, box.y + 132, box.width - 52);

    /* The bar is thin and the phase is spelled out beside it. A bar on its own
     * says how long; the words say what for, which is the part that tells you
     * it is alive rather than stuck. */
    Rectangle bar = {box.x + 26, box.y + 168, box.width - 52, 6};
    sn_progress(bar, frac, SN_ACCENT);
    sn_text(&a.ui, SN_F_TINY, phase, box.x + 26, box.y + 146, SN_DIM);

    EndDrawing();
}

void splashDone()
{
    if (g_splash.mark.id) UnloadTexture(g_splash.mark);
    if (g_splash.icon.id) UnloadTexture(g_splash.icon);
    g_splash = Splash{};
}

} /* namespace */

static int run(int argc, char **argv)
{
    /* No multisampling hint.
     *
     * It was here for the one curved thing on screen - S. Tarr's star - and
     * it cost the whole of startup on Windows. Asking for a 4x multisampled
     * pixel format makes the driver search its formats for one, and on a
     * machine whose driver has no such format that search is where the
     * program sat: measured on a Windows runner, InitWindow never returned at
     * all, and on a real machine it is the ten to fifteen seconds before the
     * window appears.
     *
     * Everything else here is rectangles and text, which multisampling does
     * nothing for. The star is drawn from triangles and is slightly harder at
     * the points without it, which is a trade worth making several times
     * over. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    mark("before window");
    if (g_timing) { printf("  ... asking for the window and a GL context\n"); fflush(stdout); }
    InitWindow(1280, 760, SN_NAME " " SN_VERSION);
    mark("window + GL");
    SetWindowMinSize(900, 560);
    SetExitKey(KEY_NULL);            /* Escape clears the selection */

    /* The font before anything else, because the splash writes with it and
     * the splash is what covers everything after this line. It costs a
     * millisecond and a half. */
    App a;
    sn_ui_init(&a.ui);
    mark("font");
    splashInit(a);
    splashDraw(a, "starting", 0.15f);
    mark("first frame");

    if (g_timing) { printf("  ... opening an audio device\n"); fflush(stdout); }
    InitAudioDevice();
    mark("audio");
    splashDraw(a, "sound", 0.35f);

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

    splashDraw(a, "decoders", 0.55f);

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
            if (!strcmp(argv[i], "--shot") || !strcmp(argv[i], "--frames")) i++;
            continue;
        }
        /* Named before it is opened, not after: probing a file on a slow
         * disk is the one part of startup that can take real time, and the
         * name of the file it is working on is the difference between a
         * progress bar and an answer. */
        splashDraw(a, GetFileName(argv[i]), 0.7f);
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

    splashDraw(a, "ready", 1.0f);
    splashDone();
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
    }

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
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("%s %s - a video editor\n\n", SN_NAME, SN_VERSION);
            printf("  bencsnip [FILE ...]\n\n");
            printf("Files on the command line are imported and laid on the timeline.\n");
            printf("A .bencsnip project file among them is opened instead.\n\n");
            printf("  --shot FILE.png    draw a few frames, write a screenshot, exit\n");
            printf("  --frames N         how many frames to draw first (default 60)\n");
            printf("  --timing           print how long each part of starting up took\n");
            return 0;
        }
    }
    return sn::run(argc, argv);
}
