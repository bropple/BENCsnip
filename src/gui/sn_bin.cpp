/*
 * BENCsnip - the media bin
 *
 * The left-hand pane: what has been dragged into the program, with a picture
 * of each so a folder of MVI_0043.MP4 is navigable at all.
 *
 * Thumbnails are made on a worker thread. Decoding one means opening the file
 * and seeking, which is tens of milliseconds for an mp4 and a great deal
 * longer for a 4K camera file over a network share, and doing that on the
 * frame a file is dropped means the window stops responding at exactly the
 * moment the user is dropping ten more.
 */

#include <cmath>
#include "sn_app.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace sn {

/* ------------------------------------------------------------------ *
 * The thumbnail worker
 * ------------------------------------------------------------------ */

namespace {

struct ThumbJob {
    int id = 0;
    std::string path;
};

struct ThumbShop {
    std::thread th;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<ThumbJob> jobs;
    std::map<int, VideoFrame> done;
    bool quit = false;
    bool started = false;
};

ThumbShop g_shop;

void thumb_worker()
{
    for (;;) {
        ThumbJob j;
        {
            std::unique_lock<std::mutex> g(g_shop.lock);
            g_shop.wake.wait(g, [] { return g_shop.quit || !g_shop.jobs.empty(); });
            if (g_shop.quit) return;
            j = g_shop.jobs.front();
            g_shop.jobs.pop_front();
        }

        VideoFrame f;
        if (thumbnail(j.path, 160, 90, &f)) {
            std::lock_guard<std::mutex> g(g_shop.lock);
            g_shop.done[j.id] = std::move(f);
        } else {
            /* An audio-only file, or one that will not decode. An empty
             * frame is a real answer: the pane draws a waveform mark instead
             * and stops asking. */
            std::lock_guard<std::mutex> g(g_shop.lock);
            g_shop.done[j.id] = VideoFrame();
        }
    }
}

void thumb_ask(int id, const std::string &path)
{
    std::lock_guard<std::mutex> g(g_shop.lock);
    if (!g_shop.started) {
        g_shop.started = true;
        g_shop.th = std::thread(thumb_worker);
    }
    g_shop.jobs.push_back(ThumbJob{id, path});
    g_shop.wake.notify_all();
}

} /* namespace */

void binShutdown(App &a)
{
    {
        std::lock_guard<std::mutex> g(g_shop.lock);
        g_shop.quit = true;
        g_shop.wake.notify_all();
    }
    if (g_shop.th.joinable()) g_shop.th.join();

    for (auto &kv : a.thumbs)
        if (kv.second.ready && kv.second.tex.id) UnloadTexture(kv.second.tex);
    a.thumbs.clear();
}

/* Pull anything the worker has finished into a texture. Uploading has to
 * happen on the thread that owns the GL context, which is this one. */
static void thumb_collect(App &a)
{
    std::map<int, VideoFrame> got;
    {
        std::lock_guard<std::mutex> g(g_shop.lock);
        if (g_shop.done.empty()) return;
        got.swap(g_shop.done);
    }

    for (auto &kv : got) {
        Thumb &t = a.thumbs[kv.first];
        if (t.ready && t.tex.id) UnloadTexture(t.tex);
        t.ready = true;
        t.tex = Texture2D{0, 0, 0, 0, 0};

        VideoFrame &f = kv.second;
        if (!f.valid()) continue;

        Image img;
        img.data = f.rgba.data();
        img.width = f.w;
        img.height = f.h;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        t.tex = LoadTextureFromImage(img);   /* copies; f can go */
    }
}

/* ------------------------------------------------------------------ *
 * The pane
 * ------------------------------------------------------------------ */

enum { ROW_H = 62, THUMB_W = 84, THUMB_H = 48 };

static void draw_audio_mark(Rectangle r, Color c)
{
    /* Four bars of a waveform. Not the file's actual waveform - drawing that
     * means decoding the whole thing - but enough to say "this is sound". */
    static const float h[] = {0.35f, 0.75f, 0.5f, 0.9f, 0.45f, 0.65f, 0.3f};
    const int n = (int)(sizeof h / sizeof h[0]);
    const float bw = r.width / (n * 2.0f);
    for (int i = 0; i < n; i++) {
        const float bh = r.height * h[i];
        DrawRectangleRec(Rectangle{r.x + bw * (i * 2 + 0.5f), r.y + (r.height - bh) * 0.5f,
                                   bw, bh},
                         c);
    }
}

/* ------------------------------------------------------------------ *
 * Drawers
 *
 * The tab at the edge and the slide. Both drawers use these; see the note on
 * Drawer in sn_app.h for why there are two of the same thing.
 * ------------------------------------------------------------------ */

bool drawerTab(App &a, Drawer &d, Rectangle tab, sn_icon icon, const char *name,
               bool left)
{
    sn_ui &ui = a.ui;

    const bool hot = CheckCollisionPointRec(GetMousePosition(), tab) && !sn_ui_blocked(&ui);

    DrawRectangleRec(tab, d.open ? SN_PANEL_HI : SN_PANEL);
    DrawLine(left ? (int)(tab.x + tab.width) - 1 : (int)tab.x, (int)tab.y,
             left ? (int)(tab.x + tab.width) - 1 : (int)tab.x,
             (int)(tab.y + tab.height), SN_BORDER);

    /* The icon a third of the way down rather than in the middle: the tab is
     * the height of the window and a mark at its centre is a mark nobody
     * looks at, because that is where the picture is. */
    Rectangle ic = {tab.x + 2, tab.y + 26, tab.width - 4, tab.width - 4};
    sn_draw_icon(icon, ic, d.open ? SN_TEXT : (hot ? SN_TEXT : SN_DIM));

    /* The name down the tab, a letter to a line. Rotated text would be
     * better and there is no rotated text here; stacked letters are what a
     * sixteen pixel tab can hold and they read at a glance. */
    float ly = ic.y + ic.height + 6;
    for (const char *p = name; *p && ly < tab.y + tab.height - 10; p++, ly += 11) {
        const char one[2] = {*p, 0};
        sn_text_center(&ui, SN_F_TINY, one, tab.x + tab.width * 0.5f, ly,
                       d.open ? SN_DIM : SN_EDGE);
    }

    if (hot) {
        sn_tip(&ui, d.open ? "click to put it away" : "click to open it");
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            d.open = !d.open;
            d.touched = GetTime();
            return true;
        }
    }
    return false;
}

void drawerStep(App &a, Drawer &d, Rectangle r, float full, bool busy)
{
    /* The pointer being in it, or a drag having started in it, is what keeps
     * it open. The drag matters: pulling a file out of the bin ends up over
     * the timeline, which is outside the drawer, and a panel that shut on the
     * way past would take the drag with it. */
    const bool inside = CheckCollisionPointRec(GetMousePosition(), r);
    if (inside || busy || d.pinned) d.touched = GetTime();

    if (d.open && !d.pinned && GetTime() - d.touched > 5.0) d.open = false;

    /* Towards where it should be, a fraction of the remaining distance per
     * frame, which is fast where it matters and slow where it lands. */
    const float want = d.open ? full : 0.0f;
    const float step = (want - d.w) * std::min(1.0f, GetFrameTime() * 14.0f);
    d.w += step;
    if (std::fabs(want - d.w) < 0.75f) d.w = want;
}

void binPane(App &a, Rectangle r)
{
    thumb_collect(a);

    sn_ui &ui = a.ui;

    DrawRectangleRec(r, SN_WELL);
    DrawLine((int)(r.x + r.width) - 1, (int)r.y, (int)(r.x + r.width) - 1,
             (int)(r.y + r.height), SN_BORDER);

    /* --- header --- */
    Rectangle head = {r.x, r.y, r.width, 26};
    DrawRectangleRec(head, SN_PANEL);
    sn_text_spaced(&ui, SN_F_SMALL, "MEDIA", head.x + SN_PAD, head.y + 5, SN_DIM);

    Rectangle addB = {head.x + head.width - 74, head.y + 3, 20, 20};
    if (sn_icon_button(&ui, 9001, addB, SN_I_PLUS, 1, 0, "add files to the bin")) {
        a.modal = MODAL_OPEN;
        fileDialogOpen(".", "");
    }

    Rectangle pin = {head.x + head.width - 50, head.y + 3, 20, 20};
    if (sn_icon_button(&ui, 9002, pin, a.bin.pinned ? SN_I_LOCK : SN_I_UNLOCK, 1,
                       a.bin.pinned,
                       a.bin.pinned ? "unpin it - it will close itself again"
                                    : "pin it open"))
        a.bin.pinned = !a.bin.pinned;

    Rectangle shut = {head.x + head.width - 26, head.y + 3, 20, 20};
    if (sn_icon_button(&ui, 9003, shut, SN_I_X, 1, 0, "close it")) {
        a.bin.open = false;
        a.bin.pinned = false;
    }
    sn_divider(r.x, head.y + head.height, r.width);

    Rectangle list = {r.x, head.y + head.height + 1, r.width,
                      r.height - head.height - 1};

    /* --- empty --- */
    if (a.proj.bin.empty()) {
        BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
        tarr(a, Vector2{list.x + list.width * 0.5f, list.y + 78}, 34, TARR_IDLE);
        sn_text_center(&ui, SN_F_SMALL, "drag video or audio in here",
                       list.x + list.width * 0.5f, list.y + 128, SN_DIM);
        sn_text_center(&ui, SN_F_TINY, "anything ffmpeg opens, which is most things",
                       list.x + list.width * 0.5f, list.y + 150, SN_EDGE);
        EndScissorMode();
        return;
    }

    /* --- scrolling --- */
    const float contentH = (float)a.proj.bin.size() * ROW_H + 6;
    const Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, list) && !sn_ui_blocked(&ui)) {
        a.binScroll -= GetMouseWheelMove() * 40.0f;
    }
    const float maxScroll = std::max(0.0f, contentH - list.height);
    a.binScroll = std::max(0.0f, std::min(a.binScroll, maxScroll));

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);

    for (size_t i = 0; i < a.proj.bin.size(); i++) {
        BinItem &b = a.proj.bin[i];
        Rectangle row = {list.x + 3, list.y + 3 + (float)i * ROW_H - a.binScroll,
                         list.width - 6, ROW_H - 4};
        if (row.y > list.y + list.height || row.y + row.height < list.y) continue;

        const bool hot = !sn_ui_blocked(&ui) && CheckCollisionPointRec(m, row) &&
                         CheckCollisionPointRec(m, list);
        const bool isSel = a.selBin == b.id;

        sn_panel(row, isSel ? SN_PANEL_HI : (hot ? SN_PANEL : SN_WELL),
                 isSel ? SN_ACCENT : SN_BORDER);

        /* --- thumbnail --- */
        Rectangle tb = {row.x + 4, row.y + (row.height - THUMB_H) * 0.5f, THUMB_W, THUMB_H};
        DrawRectangleRec(tb, SN_BG);

        Thumb &t = a.thumbs[b.id];
        if (!t.asked && !b.missing) {
            t.asked = true;
            if (b.info.hasVideo) thumb_ask(b.id, b.info.path);
            else t.ready = true;
        }

        if (t.ready && t.tex.id) {
            /* Letterboxed inside the box, same as the preview does it. */
            const float sa = (float)t.tex.width / t.tex.height;
            const float ba = tb.width / tb.height;
            Rectangle dst = tb;
            if (sa > ba) { dst.height = tb.width / sa; dst.y += (tb.height - dst.height) * 0.5f; }
            else { dst.width = tb.height * sa; dst.x += (tb.width - dst.width) * 0.5f; }
            DrawTexturePro(t.tex, Rectangle{0, 0, (float)t.tex.width, (float)t.tex.height},
                           dst, Vector2{0, 0}, 0, WHITE);
        } else if (b.missing) {
            sn_text_center(&ui, SN_F_TINY, "missing", tb.x + tb.width * 0.5f,
                           tb.y + tb.height * 0.5f - 6, SN_ALERT);
        } else if (t.ready) {
            draw_audio_mark(Rectangle{tb.x + 8, tb.y + 8, tb.width - 16, tb.height - 16},
                            SN_EDGE);
        } else {
            sn_text_center(&ui, SN_F_TINY, "...", tb.x + tb.width * 0.5f,
                           tb.y + tb.height * 0.5f - 6, SN_EDGE);
        }
        DrawRectangleLinesEx(tb, 1, SN_BORDER);

        /* --- text --- */
        const float tx = tb.x + tb.width + 8;
        const float tw = row.x + row.width - tx - 6;
        sn_text_clip(&ui, SN_F_SMALL, b.info.name.c_str(), tx, row.y + 8, tw,
                     b.missing ? SN_ALERT : SN_TEXT);

        /* What is in it, and how much of it. The channel count is here
         * because whether a file is mono, stereo or more decides whether
         * there is anything to split apart, and that is a question about the
         * file rather than about the clip it becomes. */
        const char *chans = !b.info.hasAudio ? ""
                            : b.info.chans == 1 ? "  mono"
                            : b.info.chans == 2 ? "  stereo"
                                                : nullptr;
        char chbuf[24];
        if (!chans) {
            snprintf(chbuf, sizeof chbuf, "  %d ch", b.info.chans);
            chans = chbuf;
        }

        char line[128];
        snprintf(line, sizeof line, "%s  %s%s%s", fmtTime(b.info.duration).c_str(),
                 b.info.hasVideo ? "V" : "", b.info.hasAudio ? "A" : "", chans);
        sn_text(&ui, SN_F_TINY, line, tx, row.y + 28, SN_DIM);

        if (b.info.hasVideo) {
            snprintf(line, sizeof line, "%dx%d", b.info.dispW(), b.info.dispH());
            sn_text_clip(&ui, SN_F_TINY, line, tx, row.y + 42, tw, SN_EDGE);
        } else {
            snprintf(line, sizeof line, "%s %d Hz", b.info.acodec.c_str(), b.info.rate);
            sn_text_clip(&ui, SN_F_TINY, line, tx, row.y + 42, tw, SN_EDGE);
        }

        /* --- what the mouse does here --- */
        if (hot) {
            sn_cursor(&ui, b.missing ? MOUSE_CURSOR_NOT_ALLOWED
                                     : MOUSE_CURSOR_POINTING_HAND);
            /* Why a file with an audio track shows no A: it is a track of
             * silence, and saying so here is the difference between a
             * deliberate decision and a bug somebody has to go looking for. */
            sn_tip(&ui, "%s - %s, %s.%s double-click to add it at the playhead, "
                        "or drag it down",
                   b.info.name.c_str(), b.info.container.c_str(),
                   fmtSize(b.info.bytes).c_str(),
                   b.info.silentAudio ? " its audio track is silent, so it is"
                                        " not brought in."
                                      : "");

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                a.selBin = b.id;
                if (sn_double_click(&ui, 7000 + b.id)) {
                    if (b.missing) a.complain("%s is missing - right-click to relink it",
                                              b.info.name.c_str());
                    else {
                        a.playhead = placeItem(a.proj, b.id, a.playhead);
                        a.changed();
                        a.player.seek(a.playhead);
                    }
                } else if (!b.missing) {
                    /* Arm a drag. It only becomes one once the pointer has
                     * actually moved, so a click that lands here is still a
                     * click. */
                    a.drag = DRAG_FROM_BIN;
                    a.dragBin = b.id;
                    a.dragFrom = m;
                    a.dragMoved = false;
                }
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                a.selBin = b.id;
                static const char *items[] = {"add at playhead", "relink...", "-",
                                              "remove from bin"};
                sn_menu_open(&ui, m, items, 4, 200 + b.id);
            }
        }
    }

    EndScissorMode();

    /* --- the scrollbar, only when there is something to scroll --- */
    if (maxScroll > 0) {
        const float h = list.height * (list.height / contentH);
        const float y = list.y + (list.height - h) * (a.binScroll / maxScroll);
        DrawRectangle((int)(list.x + list.width - 4), (int)y, 3, (int)h, SN_EDGE);
    }
}

} /* namespace sn */
