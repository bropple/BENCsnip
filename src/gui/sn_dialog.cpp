/*
 * BENCsnip - the modal windows
 *
 * The export dialog, the information window, a confirmation, and a file
 * browser for machines where sn_filedlg finds nothing native to ask with.
 *
 * All of them are drawn into the main window rather than opened as real
 * windows: a second OS window means a second GL context, a second event loop
 * and a platform case for each, for four dialogs that are a panel and some
 * buttons.
 */

#include "sn_app.h"
#include "sn_colordlg.h"
#include "sn_prefs.h"
#include "sn_embed.h"
#include "sn_filedlg.h"
#include "sn_version.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace sn {

/* ------------------------------------------------------------------ *
 * Shared chrome
 * ------------------------------------------------------------------ */

/* Dims everything behind, and eats the mouse so a click outside the dialog
 * does not land on the timeline. */
static Rectangle modal_frame(App &a, const char *title, float w, float h)
{
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 150});

    Rectangle r = {std::floor((GetScreenWidth() - w) * 0.5f),
                   std::floor((GetScreenHeight() - h) * 0.5f), w, h};
    sn_panel(r, SN_PANEL, SN_ACCENT);

    Rectangle head = {r.x, r.y, r.width, 28};
    sn_text_spaced(&a.ui, SN_F_BODY, title, head.x + 12, head.y + 5, SN_TEXT);
    sn_divider(r.x + 8, head.y + head.height, r.width - 16);

    return r;
}

static void label(App &a, const char *s, float x, float y)
{
    sn_text_spaced(&a.ui, SN_F_TINY, s, x, y, SN_DIM);
}

/* ------------------------------------------------------------------ *
 * The built-in file browser
 * ------------------------------------------------------------------ */

namespace {

struct Browser {
    std::string dir = ".";
    std::string name;
    std::vector<std::string> dirs, files;
    int sel = -1;
    float scroll = 0;
    bool loaded = false;
};

Browser g_br;

bool interesting(const std::string &n)
{
    /* Anything ffmpeg is likely to open, plus project files. Not a filter on
     * what can be dropped - a drop takes anything and lets libav decide - but
     * a list of a hundred RAW stills is not a list anyone wants to read. */
    static const char *ok[] = {
        ".mp4",  ".mov", ".mkv", ".webm", ".avi", ".m4v", ".mpg",  ".mpeg", ".wmv",
        ".flv",  ".ts",  ".m2ts", ".mts", ".3gp", ".ogv", ".mp3",  ".wav",  ".flac",
        ".m4a",  ".aac", ".ogg", ".opus", ".wma", ".aiff", ".aif", ".bencsnip", nullptr};

    std::string lower = n;
    for (char &c : lower) c = (char)tolower((unsigned char)c);
    for (int i = 0; ok[i]; i++) {
        const size_t l = strlen(ok[i]);
        if (lower.size() > l && lower.compare(lower.size() - l, l, ok[i]) == 0) return true;
    }
    return false;
}

void browser_load()
{
    g_br.dirs.clear();
    g_br.files.clear();
    g_br.sel = -1;
    g_br.scroll = 0;

    FilePathList list = LoadDirectoryFiles(g_br.dir.c_str());
    for (unsigned i = 0; i < list.count; i++) {
        const char *p = list.paths[i];
        const char *base = GetFileName(p);
        if (base[0] == '.') continue;               /* dotfiles stay hidden */
        if (DirectoryExists(p)) g_br.dirs.push_back(base);
        else if (interesting(base)) g_br.files.push_back(base);
    }
    UnloadDirectoryFiles(list);

    std::sort(g_br.dirs.begin(), g_br.dirs.end());
    std::sort(g_br.files.begin(), g_br.files.end());
    g_br.loaded = true;
}

} /* namespace */

void fileDialogOpen(const std::string &startDir, const std::string &suggested)
{
    g_br.dir = startDir.empty() ? "." : startDir;
    if (g_br.dir == ".") g_br.dir = GetWorkingDirectory();
    g_br.name = suggested;
    browser_load();
}

int fileDialog(App &a, Rectangle unused, const char *title, bool save, std::string *out)
{
    (void)unused;
    if (!g_br.loaded) browser_load();

    sn_ui &ui = a.ui;
    Rectangle r = modal_frame(a, title, 620, 420);
    int result = 0;

    /* --- where we are --- */
    Rectangle up = {r.x + 12, r.y + 38, 30, 22};
    if (sn_icon_button(&ui, 8100, up, SN_I_FOLDER, 1, 0, "up one folder")) {
        std::string d = g_br.dir;
        size_t s = d.find_last_of("/\\");
        if (s != std::string::npos && s > 0) g_br.dir = d.substr(0, s);
        else if (s == 0) g_br.dir = "/";
        browser_load();
    }
    Rectangle pathR = {up.x + 36, up.y, r.width - 60, 22};
    sn_panel(pathR, SN_WELL, SN_BORDER);
    sn_text_clip(&ui, SN_F_TINY, g_br.dir.c_str(), pathR.x + 6, pathR.y + 5,
                 pathR.width - 12, SN_DIM);

    /* --- the list --- */
    Rectangle list = {r.x + 12, up.y + 30, r.width - 24, r.height - 150};
    sn_panel(list, SN_WELL, SN_BORDER);

    const Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, list)) g_br.scroll -= GetMouseWheelMove() * 40;

    const int total = (int)(g_br.dirs.size() + g_br.files.size());
    const float rowH = 20;
    const float maxScroll = std::max(0.0f, total * rowH - list.height + 8);
    g_br.scroll = std::max(0.0f, std::min(g_br.scroll, maxScroll));

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    for (int i = 0; i < total; i++) {
        const bool isDir = i < (int)g_br.dirs.size();
        const std::string &n = isDir ? g_br.dirs[i] : g_br.files[i - g_br.dirs.size()];

        Rectangle row = {list.x + 2, list.y + 4 + i * rowH - g_br.scroll, list.width - 4, rowH};
        if (row.y + rowH < list.y || row.y > list.y + list.height) continue;

        const bool hot = CheckCollisionPointRec(m, row) && CheckCollisionPointRec(m, list);
        if (i == g_br.sel) DrawRectangleRec(row, SN_EDGE);
        else if (hot) DrawRectangleRec(row, SN_PANEL);

        sn_text(&ui, SN_F_SMALL, (isDir ? ("[" + n + "]").c_str() : n.c_str()), row.x + 8,
                row.y + 2, isDir ? SN_ACCENT : SN_TEXT);

        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_br.sel = i;
            if (!isDir) g_br.name = n;
            if (sn_double_click(&ui, 8200 + i)) {
                if (isDir) {
                    if (g_br.dir.empty()) g_br.dir = "/";
                    if (g_br.dir.back() != '/') g_br.dir += "/";
                    g_br.dir += n;
                    browser_load();
                } else {
                    *out = g_br.dir + "/" + n;
                    result = 1;
                }
            }
        }
    }
    EndScissorMode();

    /* --- the name, when saving --- */
    Rectangle nameR = {r.x + 12, list.y + list.height + 10, r.width - 24, 24};
    if (save) {
        label(a, "FILE NAME", nameR.x, nameR.y - 12);
        nameR.y += 4;
        sn_field(&ui, 8300, nameR, g_br.name, "name.mp4");
    }

    /* --- buttons --- */
    Rectangle ok = {r.x + r.width - 200, r.y + r.height - 40, 88, 26};
    Rectangle no = {r.x + r.width - 104, r.y + r.height - 40, 88, 26};

    if (sn_button_lit(&ui, 8400, ok, save ? "SAVE" : "OPEN", 1)) {
        if (!g_br.name.empty()) {
            *out = g_br.dir + "/" + g_br.name;
            result = 1;
        }
    }
    if (sn_button(&ui, 8401, no, "CANCEL", 1) || IsKeyPressed(KEY_ESCAPE)) result = -1;
    if (IsKeyPressed(KEY_ENTER) && !g_br.name.empty()) {
        *out = g_br.dir + "/" + g_br.name;
        result = 1;
    }

    if (result != 0) g_br.loaded = false;
    return result;
}

/* ------------------------------------------------------------------ *
 * Export
 * ------------------------------------------------------------------ */

namespace {

struct Preset {
    const char *label;
    const char *ext;
    /* Encoders in order of preference, ending in a null. Which of them a
     * build of ffmpeg actually has is not knowable at compile time: a
     * distribution package has libx264, an LGPL-only static build does not,
     * and the difference should be a slightly different file rather than a
     * dialog that fails after the user has chosen a filename. An empty first
     * entry means the preset has no stream of that kind at all. */
    const char *video[4];
    const char *audio[4];
};

/* What someone actually exports. H.264 in mp4 first because it plays
 * everywhere, and that is the only property most exports need. */
const Preset PRESETS[] = {
    {"MP4", "mp4", {"libx264", "libopenh264", "mpeg4", nullptr}, {"aac", nullptr}},
    {"WEBM", "webm", {"libvpx-vp9", "libvpx", nullptr}, {"libopus", "libvorbis", nullptr}},
    {"MKV", "mkv", {"libx264", "libopenh264", "ffv1", nullptr}, {"aac", "flac", nullptr}},
    {"GIF", "gif", {"gif", nullptr}, {nullptr}},
    {"MP3", "mp3", {nullptr}, {"libmp3lame", nullptr}},
    {"WAV", "wav", {nullptr}, {"pcm_s16le", nullptr}},
};
const int NPRESET = (int)(sizeof PRESETS / sizeof PRESETS[0]);

/* The first of a list this build has, or "" for none. */
const char *pick(const char *const *names)
{
    for (int i = 0; names[i]; i++)
        if (haveEncoder(names[i])) return names[i];
    return "";
}

/* A preset is usable when everything it needs is present: an audio-only
 * format with no encoder for it is a button that can only disappoint. A
 * format with no sound at all - which is GIF, and only GIF - is judged on its
 * picture alone. */
bool usable(const Preset &p)
{
    if (p.video[0] && !*pick(p.video)) return false;
    if (p.audio[0] && !*pick(p.audio)) return false;
    return p.video[0] || p.audio[0];
}

int g_preset = 0;
int g_res = 0;         /* 0 source, 1 1080p, 2 720p, 3 480p */
float g_quality = 0.6f;
int g_gifFps = 1;      /* index into GIF_FPS below */
const double GIF_FPS[] = {10, 15, 20, 25};
std::string g_path;
bool g_pathEdited = false;

std::string swap_ext(const std::string &p, const char *ext)
{
    size_t dot = p.find_last_of('.');
    size_t slash = p.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return p.substr(0, dot) + "." + ext;
    return p + "." + ext;
}

} /* namespace */

void startExport(App &a)
{
    if (a.exStatus.running.load()) return;

    if (a.exThread && a.exThread->joinable()) a.exThread->join();
    a.exStatus.cancel.store(false);
    a.exStatus.progress.store(0.0);
    a.exStatus.say("starting");

    /* The export gets its own copy of the project and its own decoders, so
     * the timeline stays editable while it runs. */
    Project copy = a.proj;
    ExportSettings s = a.ex;
    a.exThread.reset(new std::thread([&a, copy, s]() { exportTimeline(copy, s, &a.exStatus); }));
}

void exportDialog(App &a)
{
    sn_ui &ui = a.ui;
    const bool running = a.exStatus.running.load();

    Rectangle r = modal_frame(a, running ? "EXPORTING" : "EXPORT", 560, 396);

    /* --- while it runs, the dialog is a progress bar --- */
    if (running) {
        tarr(a, Vector2{r.x + r.width * 0.5f, r.y + 96}, 34, TARR_BUSY);

        const float p = (float)a.exStatus.progress.load();
        Rectangle bar = {r.x + 40, r.y + 160, r.width - 80, 18};
        sn_progress(bar, p, SN_ACCENT);

        char line[64];
        snprintf(line, sizeof line, "%d%%", (int)(p * 100));
        sn_text_center(&ui, SN_F_BODY, line, r.x + r.width * 0.5f, bar.y + 26, SN_TEXT);

        sn_text_center(&ui, SN_F_SMALL, a.exStatus.said().c_str(), r.x + r.width * 0.5f,
                       bar.y + 52, SN_DIM);
        sn_text_center(&ui, SN_F_TINY, GetFileName(a.ex.path.c_str()),
                       r.x + r.width * 0.5f, bar.y + 74, SN_EDGE);

        Rectangle stop = {r.x + r.width * 0.5f - 50, r.y + r.height - 44, 100, 26};
        if (sn_button(&ui, 8500, stop, "CANCEL", 1)) a.exStatus.cancel.store(true);
        return;
    }

    /* --- a finished run, still on screen --- */
    if (a.exThread && a.exThread->joinable()) {
        a.exThread->join();
        a.exThread.reset();
        if (a.exStatus.ok.load()) {
            a.say("wrote %s%s", GetFileName(a.ex.path.c_str()),
                  a.exStatus.copied.load() ? " without re-encoding" : "");
            a.modal = MODAL_NONE;
            return;
        }
        a.complain("export failed: %s", a.exStatus.said().c_str());
    }

    /* --- the settings --- */
    float y = r.y + 42;

    /* Format. */
    label(a, "FORMAT", r.x + 16, y);
    y += 14;
    for (int i = 0; i < NPRESET; i++) {
        Rectangle b = {r.x + 16 + i * 76.0f, y, 70, 24};
        if (!usable(PRESETS[i])) {
            sn_panel(b, SN_PANEL, SN_PANEL_HI);
            sn_text_center(&ui, SN_F_SMALL, PRESETS[i].label, b.x + b.width * 0.5f,
                           b.y + 4, SN_EDGE);
            continue;
        }
        if (sn_toggle(&ui, 8600 + i, b, PRESETS[i].label, g_preset == i)) {
            g_preset = i;
            g_path = swap_ext(g_path, PRESETS[i].ext);
        }
    }
    y += 34;

    /* Where it goes. */
    label(a, "SAVE TO", r.x + 16, y);
    y += 14;
    Rectangle pathR = {r.x + 16, y, r.width - 32 - 76, 24};
    if (sn_field(&ui, 8610, pathR, g_path, "output.mp4")) g_pathEdited = true;
    Rectangle browse = {pathR.x + pathR.width + 6, y, 70, 24};
    if (sn_button(&ui, 8611, browse, "BROWSE", 1)) {
        char buf[1024] = {0};
        int rc = sn_save_dialog(GetWindowHandle(), "Export to", GetFileName(g_path.c_str()),
                                "Video", PRESETS[g_preset].ext, buf, sizeof buf);
        if (rc == SN_DLG_OK) { g_path = buf; g_pathEdited = true; }
        else if (rc == SN_DLG_UNAVAILABLE)
            a.complain("no zenity or kdialog here - type the path instead");
    }
    y += 34;

    /* Size. */
    const bool videoOut = PRESETS[g_preset].video[0] != nullptr;
    const bool isGif = videoOut && !strcmp(PRESETS[g_preset].video[0], "gif");
    if (videoOut) {
        label(a, "SIZE", r.x + 16, y);
        y += 14;
        static const char *sizes[] = {"SOURCE", "1080P", "720P", "480P"};
        for (int i = 0; i < 4; i++) {
            Rectangle b = {r.x + 16 + i * 76.0f, y, 70, 24};
            if (sn_toggle(&ui, 8620 + i, b, sizes[i], g_res == i)) g_res = i;
        }
        y += 34;

        if (isGif) {
            /* A GIF has no quality setting - it is 256 colours whatever
             * happens - so the knob that decides how big it comes out is the
             * frame rate. 15 to start: high enough to look like motion, and
             * half the frames of the video it came from. */
            label(a, "FRAME RATE", r.x + 16, y);
            y += 14;
            for (int i = 0; i < 4; i++) {
                char lab[24];
                snprintf(lab, sizeof lab, "%d FPS", (int)GIF_FPS[i]);
                Rectangle b = {r.x + 16 + i * 76.0f, y, 70, 24};
                if (sn_toggle(&ui, 8640 + i, b, lab, g_gifFps == i)) g_gifFps = i;
            }
            y += 30;
        } else {
            /* Quality. */
            label(a, "QUALITY", r.x + 16, y);
            y += 14;
            Rectangle sl = {r.x + 16, y, r.width - 200, 20};
            sn_slider(&ui, 8630, sl, &g_quality);

            /* crf 34..14 across the slider: lower is better, and nobody should
             * have to know that. */
            const int crf = (int)std::lround(34 - g_quality * 20);
            char q[96];
            snprintf(q, sizeof q, "%s  (crf %d)",
                     g_quality < 0.3f ? "small file" : g_quality > 0.8f ? "near-lossless"
                                                                        : "good", crf);
            sn_text(&ui, SN_F_TINY, q, sl.x + sl.width + 10, sl.y + 4, SN_DIM);
            y += 30;
        }
    }

    /* --- what it is going to do --- */
    a.ex.path = g_path;
    a.ex.vcodec = PRESETS[g_preset].video[0] ? pick(PRESETS[g_preset].video) : "";
    a.ex.acodec = pick(PRESETS[g_preset].audio);
    a.ex.fps = isGif ? GIF_FPS[g_gifFps] : a.proj.fps;
    a.ex.crf = videoOut ? (int)std::lround(34 - g_quality * 20) : 20;
    a.ex.from = 0;
    a.ex.to = -1;

    switch (g_res) {
    case 1: a.ex.height = 1080; break;
    case 2: a.ex.height = 720; break;
    case 3: a.ex.height = 480; break;
    default: a.ex.height = a.proj.height; break;
    }
    if (g_res == 0) {
        a.ex.width = a.proj.width;
    } else {
        /* Keep the project's aspect rather than assuming 16:9 - a vertical
         * phone video exported at "1080p" should be 1080 tall, not squashed
         * into a landscape frame. */
        const double ar = a.proj.height > 0 ? (double)a.proj.width / a.proj.height : 16.0 / 9.0;
        a.ex.width = (int)std::lround(a.ex.height * ar) & ~1;
    }

    std::string why;
    const bool fast = canStreamCopy(a.proj, a.ex, &why);

    bool exact = false;
    const int64_t guess = estimateSize(a.proj, a.ex, &exact);

    Rectangle note = {r.x + 16, r.y + r.height - 78, r.width - 32, 30};
    sn_panel(note, SN_WELL, SN_BORDER);
    if (fast) {
        sn_text(&ui, SN_F_TINY, "fast trim: the file is copied, not re-encoded.",
                note.x + 8, note.y + 4, SN_ACCENT);
        sn_text(&ui, SN_F_TINY, "it starts at the keyframe before the in point.",
                note.x + 8, note.y + 16, SN_DIM);
    } else {
        char l1[160];
        snprintf(l1, sizeof l1, "rendering %s of timeline at %dx%d, %.2f fps",
                 fmtTime(a.proj.duration()).c_str(), a.ex.width, a.ex.height, a.ex.fps);
        sn_text(&ui, SN_F_TINY, l1, note.x + 8, note.y + 4, SN_TEXT);
        snprintf(l1, sizeof l1, "no fast trim: %s", why.c_str());
        sn_text(&ui, SN_F_TINY, l1, note.x + 8, note.y + 16, SN_DIM);
    }

    /* The size, on the right of the same panel. "about" is doing real work
     * there: at constant quality the encoder decides the bitrate from the
     * picture, and a still frame and a snowstorm at the same setting are not
     * the same file. A copy is exact, and says so by not saying about. */
    {
        char sz[64];
        snprintf(sz, sizeof sz, "%s%s", exact ? "" : "about ", fmtSize(guess).c_str());
        const float w = sn_measure(&ui, SN_F_SMALL, sz, 0.0f);
        sn_text(&ui, SN_F_SMALL, sz, note.x + note.width - w - 10, note.y + 8,
                exact ? SN_ACCENT : SN_TEXT);
    }

    /* --- go --- */
    const bool ready = !g_path.empty() && a.proj.duration() > 0;
    Rectangle go = {r.x + r.width - 200, r.y + r.height - 40, 88, 26};
    Rectangle no = {r.x + r.width - 104, r.y + r.height - 40, 88, 26};

    if (sn_button_lit(&ui, 8700, go, "EXPORT", ready) && ready) startExport(a);
    if (sn_button(&ui, 8701, no, "CANCEL", 1) || IsKeyPressed(KEY_ESCAPE))
        a.modal = MODAL_NONE;
}

/* Called once, when the dialog opens, so the path field starts somewhere
 * sensible without overwriting what the user typed last time. */
void exportRenamed(App &a)
{
    /* A filename someone typed is theirs; a suggested one is ours to keep up
     * to date. */
    if (g_pathEdited) return;
    g_path.clear();
    exportDialogPrepare(a);
}

void exportDialogPrepare(App &a)
{
    /* Land on something this build can actually write. */
    if (!usable(PRESETS[g_preset]))
        for (int i = 0; i < NPRESET; i++)
            if (usable(PRESETS[i])) { g_preset = i; break; }

    if (g_pathEdited && !g_path.empty()) return;

    std::string base = a.proj.name;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    if (base.empty() || base == "untitled") {
        /* Named after the first thing on the timeline, which is nearly always
         * what the export is of. */
        for (const Track &t : a.proj.tracks)
            if (!t.clips.empty()) {
                const BinItem *b = a.proj.item(t.clips[0].source);
                if (b) {
                    base = b->info.name;
                    size_t d2 = base.find_last_of('.');
                    if (d2 != std::string::npos) base = base.substr(0, d2);
                    base += "-snip";
                }
                break;
            }
    }
    if (base.empty()) base = "export";

    std::string dir = GetWorkingDirectory();
    if (!a.proj.path.empty()) {
        size_t s = a.proj.path.find_last_of("/\\");
        if (s != std::string::npos) dir = a.proj.path.substr(0, s);
    }
    g_path = dir + "/" + base + "." + PRESETS[g_preset].ext;
}


/* ------------------------------------------------------------------ *
 * One track's layout
 *
 * Size, position and crop, per video track, which is what makes a small
 * video inside a big one or two side by side. Everything is relative to the
 * canvas, so changing the project's size later moves nothing.
 *
 * The window draws what it is describing, at the canvas's own aspect, with
 * this track's rectangle in it. Four numbers and a preview beats four numbers.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * The caption window
 *
 * What a caption says and how it looks. Where it sits, how big it is and how
 * far it is turned are here too, but the preview is the better place to set
 * those - dragging the thing itself beats typing a number at it - so this
 * says so and keeps the numbers for when a number is what you actually want.
 *
 * The preview pane is still on screen behind this, dimmed rather than hidden,
 * and every control marks the change minor, so the caption behind the window
 * updates as it is edited. That is the whole reason there is no second little
 * preview drawn inside the dialog: there is already a real one, at the real
 * size, showing the real frame.
 * ------------------------------------------------------------------ */

namespace {

/* The words, split into the lines the dialog edits and joined back up.
 *
 * Three single-line fields rather than one that takes newlines. sn_field is a
 * one-line field with a caret and a selection in it, and teaching it to wrap
 * is a bigger piece of work than captions justify - almost none run past
 * three lines, and the ones that do are a title card somebody should be
 * setting in something else. The renderer and the project file take any
 * number; this is the editor being modest, not the model.
 */
void split_lines(const std::string &text, std::string out[3])
{
    int n = 0;
    std::string cur;
    for (char ch : text) {
        if (ch == '\n') {
            if (n < 3) out[n++] = cur;
            cur.clear();
            if (n >= 3) return;
        } else {
            cur += ch;
        }
    }
    if (n < 3) out[n] = cur;
}

std::string join_lines(const std::string in[3])
{
    /* Trailing empties are dropped, so clearing the second line does not
     * leave a caption with a blank row under it that nothing on screen
     * explains. An empty line between two full ones is kept, because that one
     * was asked for. */
    int last = -1;
    for (int i = 0; i < 3; i++)
        if (!in[i].empty()) last = i;

    std::string out;
    for (int i = 0; i <= last; i++) {
        if (i) out += '\n';
        out += in[i];
    }
    return out;
}

/* --- one colour on one line ---
 *
 * A swatch, the hex somebody can type or read off, the platform's own picker,
 * and alpha on its own.
 *
 * It was four sliders per colour - red, green, blue, alpha - laid out like the
 * crop row in the layout window. Nobody picks a colour by moving three numbers
 * separately, and eight sliders were also a hundred pixels wider than the
 * window they were in, which is how the arrangement announced itself.
 *
 * Alpha stays a slider because it is the one channel the pickers disagree
 * about: ChooseColor has never heard of it, zenity sometimes answers with one,
 * and only NSColorPanel does it properly. A control that works on one platform
 * out of three is worse than a slider that works on all of them.
 */
bool hex_of(Rgba c, char *out, size_t cap)
{
    return snprintf(out, cap, "%02X%02X%02X", (unsigned)((c >> 24) & 0xff),
                    (unsigned)((c >> 16) & 0xff), (unsigned)((c >> 8) & 0xff)) > 0;
}

/* "#1a2b3c", "1a2b3c", "#1a2b3cff", and the three-digit short form every web
 * page uses. Anything else leaves the colour alone: a half-typed hex is a
 * person still typing, not a request for black. */
bool hex_to(const std::string &in, Rgba *c)
{
    std::string t;
    for (char ch : in)
        if (!isspace((unsigned char)ch) && ch != '#') t += (char)toupper((unsigned char)ch);

    for (char ch : t)
        if (!isxdigit((unsigned char)ch)) return false;

    auto nib = [](char ch) {
        return ch <= '9' ? ch - '0' : ch - 'A' + 10;
    };

    unsigned r, g, b, a = *c & 0xffu;
    if (t.size() == 3) {
        r = (unsigned)nib(t[0]) * 17; g = (unsigned)nib(t[1]) * 17;
        b = (unsigned)nib(t[2]) * 17;
    } else if (t.size() == 6 || t.size() == 8) {
        r = (unsigned)(nib(t[0]) * 16 + nib(t[1]));
        g = (unsigned)(nib(t[2]) * 16 + nib(t[3]));
        b = (unsigned)(nib(t[4]) * 16 + nib(t[5]));
        if (t.size() == 8) a = (unsigned)(nib(t[6]) * 16 + nib(t[7]));
    } else {
        return false;
    }

    *c = (r << 24) | (g << 16) | (b << 8) | a;
    return true;
}

/* What is in each hex field, kept between frames.
 *
 * A field being typed into holds what has been typed, half-finished and all;
 * a field that does not have the caret shows the colour. Without that, every
 * keystroke would be overwritten by the colour the field has not yet been
 * told about, and "1a2b3" - five digits of six - would never survive long
 * enough to become six.
 */
std::map<int, std::string> g_hex;

/* One colour, in two lines, because one line does not fit.
 *
 *   FILL     [swatch] [ 296600      ]
 *            [ PICK ]  A [------] 100%
 *
 * It was one line, laid out for a dialog five hundred and eighty pixels wide.
 * In a three hundred pixel column the picker button and the alpha slider ran
 * off the right-hand edge and the panel looked like it had been squashed -
 * which it had. Two lines is what the width allows, and the button goes on
 * the second because it is the one thing here that opens something else. */
bool colour_row(App &a, int id, const char *name, Rgba *c, float x, float y, float w)
{
    sn_ui &ui = a.ui;
    label(a, name, x, y + 4);

    bool changed = false;

    const unsigned r = (*c >> 24) & 0xff, g = (*c >> 16) & 0xff;
    const unsigned b = (*c >> 8) & 0xff, al = *c & 0xff;

    /* The swatch, over a chequer so an alpha of nothing looks like nothing
     * rather than like black. */
    Rectangle sw = {x + 72, y, 30, 21};
    for (int gy = 0; gy < 3; gy++)
        for (int gx = 0; gx < 4; gx++)
            DrawRectangle((int)sw.x + gx * 8, (int)sw.y + gy * 8, 8, 8,
                          (gx + gy) % 2 ? SN_EDGE : SN_BG);
    DrawRectangleRec(sw, Color{(unsigned char)r, (unsigned char)g, (unsigned char)b,
                               (unsigned char)al});
    DrawRectangleLinesEx(sw, 1, SN_BORDER);

    std::string &text = g_hex[id];
    if (ui.focus != id) {
        char now[16];
        hex_of(*c, now, sizeof now);
        text = now;
    }

    Rectangle hf = {x + 108, y, w - 108, 21};
    if (sn_field(&ui, id, hf, text, "RRGGBB")) {
        Rgba n = *c;
        if (hex_to(text, &n) && n != *c) { *c = n; changed = true; }
    }

    /* --- the second line --- */
    const float y2 = y + 25;

    Rectangle pick = {x + 72, y2, 58, 20};
    if (sn_button(&ui, id + 1, pick, "PICK", 1)) {
        unsigned v = (unsigned)*c;
        const int rc = sn_color_dialog(GetWindowHandle(), name, &v);
        if (rc == SN_COLOR_OK && (Rgba)v != *c) {
            *c = (Rgba)v;
            changed = true;
        } else if (rc == SN_COLOR_UNAVAILABLE) {
            a.complain("no colour picker on this machine - type the hex instead");
        }
    }

    sn_text(&ui, SN_F_TINY, "A", x + 138, y2 + 4, SN_DIM);
    Rectangle as = {x + 150, y2 + 4, w - 150 - 34, 13};
    float av = al / 255.0f;
    if (sn_slider(&ui, id + 2, as, &av)) {
        *c = (*c & 0xffffff00u) | (Rgba)(unsigned)(av * 255.0f + 0.5f);
        changed = true;
    }
    {
        char n[16];
        snprintf(n, sizeof n, "%d%%", (int)(al * 100 / 255));
        sn_text(&ui, SN_F_TINY, n, as.x + as.width + 6, y2 + 4, SN_EDGE);
    }

    return changed;
}

/* The font list's search box and where it is scrolled to. The panel's own
 * state rather than the project's - closing it forgets both, which is what
 * you want from a search box. */
std::string g_fontFilter;
float g_fontScroll = 0;

} /* namespace */

/* ------------------------------------------------------------------ *
 * The controls
 *
 * Every control this program has, in one table.
 *
 * One table rather than prose, and in the program rather than only in the
 * README, because the README is not open when somebody is wondering what the
 * key for the thing is. It is one array so that adding a control means adding
 * a row: two lists of the same facts drift apart, and the one nobody is
 * looking at is always the wrong one.
 *
 * A row with no key is a heading. A row with no text is a blank line.
 * ------------------------------------------------------------------ */

namespace {

struct HelpRow {
    const char *key;
    const char *what;
};

const HelpRow HELP[] = {
    {nullptr, "PLAYING"},
    {"Space", "play or pause"},
    {"Home / End", "to the beginning, or the end"},
    {"< and >", "to the previous cut, or the next"},
    {"arrows", "one frame; hold Shift for a second"},
    {"", nullptr},

    {nullptr, "FILES"},
    {"Ctrl+N", "start again"},
    {"Ctrl+O", "open a project"},
    {"Ctrl+S", "save it; Ctrl+Shift+S to save it somewhere else"},
    {"Ctrl+I", "add media to the bin"},
    {"Ctrl+E", "export"},
    {"drop a file", "on the window to import it; drop a .bencsnip to open it"},
    {"", nullptr},

    {nullptr, "EDITING"},
    {"S", "split every track at the playhead"},
    {"Delete", "delete the selected clip - Backspace does it too"},
    {"Shift+Delete", "delete it and close the gap"},
    {"M", "mute the selected clip"},
    {"Ctrl+Z", "undo; Ctrl+Shift+Z to redo"},
    {"Ctrl+A", "select everything; Esc to select nothing"},
    {"Ctrl+T", "put a caption at the playhead"},
    {"", nullptr},

    {nullptr, "THE VIEW"},
    {"F", "fit the whole timeline in the window"},
    {"+ and -", "zoom in and out"},
    {"wheel", "scroll the tracks; Shift for sideways, Ctrl to zoom"},
    {"middle-drag", "pan sideways"},
    {"F12", "write a screenshot beside the program"},
    {"", nullptr},

    {nullptr, "THE TIMELINE, WITH THE MOUSE"},
    {"click a clip", "select it, and show its track in the panel"},
    {"drag its middle", "move it, to another track if you like"},
    {"drag an edge", "trim it - past the end of the source to loop it"},
    {"the ruler", "scrub. It is the only thing that moves the playhead"},
    {"right-click", "split, delete, mute, unlink, split channels"},
    {"double-click", "a caption, for the panel with its words in it"},
    {"", nullptr},

    {nullptr, "THE EFFECTS LANE, UNDER EVERY TRACK"},
    {"click", "add a point; drag it to say when and how much"},
    {"Shift+drag", "mark out a stretch for a preset to cover"},
    {"right-click", "fade in, fade out, in and out, pulse, wave"},
    {"", "and to hold a point, delete it, or clear the lane"},
    {"", "a held point steps instead of sliding - that is a square wave"},
    {"", nullptr},

    {nullptr, "THE TWO PANELS"},
    {"the tabs", "MEDIA on the left, SETUP on the right - click to open"},
    {"", "they slide out beside the picture rather than over it"},
    {"", "and close themselves five seconds after you leave"},
    {"the pin", "in a panel's header keeps it open for good"},
    {"", "how you leave them is how they come back next time"},
    {"Esc", "select nothing - clips, pictures and captions all at once"},
    {"every slider", "has a box beside it - drag for roughly, type for exactly"},
    {"", nullptr},

    {nullptr, "THE PREVIEW"},
    {"click a layer", "select it; drag it about; drag a corner to resize"},
    {"Tab", "the next layer down, where several overlap"},
    {"double-click", "its numbers in the panel - crop, size, position, mirrors"},
    {"R", "turn the selected caption 15 degrees; Shift+R the other way"},
    {"C", "the panel, on the selected picture"},
    {"the ball", "above a selected caption turns it; Shift steps by 15"},
    {"", nullptr},

    {nullptr, "TRACKS"},
    {"+V  +A", "add a video or an audio track. New ones go below"},
    {"the arrows", "move a track up or down. The top row is the BACK"},
    {"click a head", "show that track in the panel on the right"},
    {"the switches", "hide or mute it, lock it, delete it"},
    {"the fader", "an audio track's level, on top of each clip's"},
};

float g_helpScroll = 0;
std::string g_helpFind;

/* Case-insensitive "does this row contain what was typed". */
bool help_matches(const HelpRow &row, const std::string &needle)
{
    if (needle.empty()) return true;

    std::string hay;
    if (row.key) hay += row.key;
    hay += ' ';
    if (row.what) hay += row.what;
    for (char &c : hay) c = (char)tolower((unsigned char)c);

    std::string want = needle;
    for (char &c : want) c = (char)tolower((unsigned char)c);
    return hay.find(want) != std::string::npos;
}

} /* namespace */

void helpDialog(App &a)
{
    sn_ui &ui = a.ui;

    Rectangle r = modal_frame(a, "CONTROLS", 620, 560);

    /* A search box, because a table of eighty rows is a table somebody
     * scrolls past the thing they were looking for. Typing narrows it to the
     * rows that match, and the heading each surviving row sits under comes
     * with it - a shortcut with no idea what part of the program it belongs
     * to is half an answer. */
    Rectangle find = {r.x + 10, r.y + 34, r.width - 20, 22};
    sn_field(&ui, 8901, find, g_helpFind, "type to find a control");
    ui.focus = 8901;

    Rectangle body = {r.x + 10, find.y + find.height + 6, r.width - 20,
                      r.height - (find.y + find.height + 6 - r.y) - 48};
    sn_panel(body, SN_WELL, SN_BORDER);

    const int n = (int)(sizeof HELP / sizeof HELP[0]);
    const float rowH = 15.0f;

    /* Which rows survive the search, and which headings still have anything
     * under them. Worked out first so the height is known before drawing. */
    std::vector<char> show((size_t)n, 0);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const HelpRow &row = HELP[i];
        if (!row.key && row.what) continue;              /* a heading: later */
        if (!row.key && !row.what) continue;
        if (help_matches(row, g_helpFind)) { show[(size_t)i] = 1; shown++; }
    }
    for (int i = 0; i < n; i++) {
        if (HELP[i].key || !HELP[i].what) continue;      /* only headings */
        for (int j = i + 1; j < n && HELP[j].what != nullptr; j++)
            if (show[(size_t)j]) { show[(size_t)i] = 1; break; }
    }

    float want = 16;
    for (int i = 0; i < n; i++)
        if (show[(size_t)i]) want += rowH;

    if (CheckCollisionPointRec(GetMousePosition(), body) && !sn_ui_blocked(&ui))
        g_helpScroll -= GetMouseWheelMove() * 40.0f;
    g_helpScroll = std::max(0.0f, std::min(g_helpScroll, std::max(0.0f, want - body.height)));

    BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);
    float ly = body.y + 8 - g_helpScroll;
    for (int i = 0; i < n; i++) {
        if (!show[(size_t)i]) continue;
        const HelpRow &row = HELP[i];

        if (ly > body.y - rowH && ly < body.y + body.height) {
            if (!row.key) {
                sn_text_spaced(&ui, SN_F_TINY, row.what, body.x + 8, ly, SN_ACCENT);
            } else if (row.what) {
                /* A row that matched what was typed is lit, so that in a
                 * section which survived because of one line, that line is
                 * the one your eye lands on. */
                const bool hit = !g_helpFind.empty() && help_matches(row, g_helpFind);
                if (row.key[0])
                    sn_text(&ui, SN_F_TINY, row.key, body.x + 14, ly,
                            hit ? SN_ACCENT : SN_TEXT);
                sn_text_clip(&ui, SN_F_TINY, row.what, body.x + 150, ly,
                             body.width - 160,
                             hit ? SN_TEXT : (row.key[0] ? SN_DIM : SN_EDGE));
            }
        }
        ly += rowH;
    }
    EndScissorMode();

    if (shown == 0)
        sn_text(&ui, SN_F_TINY, "nothing here matches that", body.x + 10, body.y + 10,
                SN_EDGE);

    sn_text(&ui, SN_F_TINY,
            g_helpFind.empty() ? "scroll for the rest" : "Esc clears the search",
            r.x + 12, r.y + r.height - 34, SN_EDGE);

    Rectangle close = {r.x + r.width - 104, r.y + r.height - 40, 88, 26};
    const bool done = sn_button_lit(&ui, 8900, close, "DONE", 1) != 0;

    /* Escape empties the search first and closes the window second, which is
     * the order somebody who has just searched for the wrong word wants. */
    if (IsKeyPressed(KEY_ESCAPE) && !g_helpFind.empty()) {
        g_helpFind.clear();
        g_helpScroll = 0;
    } else if (done || IsKeyPressed(KEY_ESCAPE)) {
        a.modal = MODAL_NONE;
        ui.focus = 0;
    }
}

/* ------------------------------------------------------------------ *
 * The inspector
 *
 * What is selected, on the right, in a column rather than in a window over
 * the middle of the screen.
 *
 * It was two modal dialogs. A modal is the wrong shape for this: adjusting a
 * layer's size or a caption's colour is something you do *while looking at
 * the picture*, and a window in the middle of the screen covers the picture.
 * Worse, it stops the transport, the timeline and everything else until it is
 * dismissed, so checking what a change looks like a second later meant
 * closing it, scrubbing, and opening it again.
 *
 * One column, everything stacked in one narrow lane, scrolled when it does
 * not fit. Narrow is what makes it fit beside the preview rather than over
 * it, and a single lane is what makes narrow work: two columns of controls in
 * three hundred pixels is two columns of six characters.
 * ------------------------------------------------------------------ */

namespace {

/* The vertical cursor a panel body is laid out with. Everything here is
 * "put the next thing under the last thing", which is the whole reason a
 * column can hold controls a dialog had to arrange by hand. */
/* What each number field has in it while it is being typed into. Keyed by
 * the same id the slider and the field share, so there is one of these per
 * control and no way for two to be confused. */
std::map<int, std::string> g_num;

/* Everything in a number field that is not part of a number, removed as it is
 * typed. Digits, one sign at the front, one dot anywhere.
 *
 * The value was already safe without this - a parse that fails leaves it
 * alone - but "only numbers go in the box" is a better rule than "letters go
 * in and are quietly ignored", because the second one looks like the field
 * accepted something and then did nothing about it.
 *
 * No exponent. strtod would take "1e3", and nobody types that into a caption
 * size; leaving 'e' out means the letters are simply all gone. */
void keep_numeric(std::string &t)
{
    std::string out;
    bool dot = false;
    for (char ch : t) {
        if (ch >= '0' && ch <= '9') out += ch;
        else if ((ch == '-' || ch == '+') && out.empty()) out += ch;
        else if (ch == '.' && !dot) { out += ch; dot = true; }
    }
    t.swap(out);
}

struct Lane {
    App *a;
    float x, w, y;

    void gap(float h = 10) { y += h; }

    void title(const char *s)
    {
        sn_text_spaced(&a->ui, SN_F_SMALL, s, x, y, SN_ACCENT);
        y += 18;
    }

    void note(const char *s)
    {
        sn_text_clip(&a->ui, SN_F_TINY, s, x, y, w, SN_EDGE);
        y += 13;
    }

    void label(const char *s)
    {
        sn_text_spaced(&a->ui, SN_F_TINY, s, x, y, SN_DIM);
        y += 13;
    }

    /* A slider with its name above it and its value beside it, and the value
     * is a field you can type into.
     *
     * A slider is quick and cannot be exact: it is one pixel to about a
     * hundredth of the range, and "0.12" is not a thing a hand lands on. Both,
     * then - drag for roughly, type for exactly - and the field is the same
     * width as the number it holds so it does not look like somewhere to
     * write an essay.
     *
     * While the caret is in it the text is whatever is being typed, and it
     * goes back to the value the moment it is not. Otherwise "0.1" on the way
     * to "0.15" would be rewritten as "0.10" and the next keystroke would
     * make it "0.101".
     */
    bool number(int id, const char *name, double *v, double lo, double hi,
                const char *fmt)
    {
        label(name);

        Rectangle sl = {x, y + 3, w - 66, 14};
        float t = (float)((*v - lo) / (hi - lo));
        bool moved = sn_slider(&a->ui, id, sl, &t) != 0;
        if (moved) *v = lo + t * (hi - lo);

        std::string &text = g_num[id];
        char now[48];
        snprintf(now, sizeof now, fmt, *v);
        if (a->ui.focus != id || moved) text = now;

        Rectangle fld = {x + w - 60, y, 60, 20};
        if (sn_field(&a->ui, id, fld, text, "")) {
            const size_t was = text.size();
            keep_numeric(text);
            if (text.size() != was && a->ui.caret > (int)text.size())
                a->ui.caret = a->ui.anchor = (int)text.size();

            /* Anything that is not a number leaves the value alone: somebody
             * halfway through typing "-" or "0." has not asked for zero.
             *
             * And the result has to be a real number. strtod takes "nan",
             * and a NaN survives being clamped - both `got < lo` and
             * `got > hi` are false for it - so without this check it would
             * go straight into a caption's size, out to the renderer, and
             * into the project file, where it would still be a NaN the next
             * time the file was opened. */
            const char *p = text.c_str();
            char *end = nullptr;
            const double got = strtod(p, &end);
            if (end && end != p && std::isfinite(got)) {
                *v = got < lo ? lo : (got > hi ? hi : got);
                moved = true;
            }
        }

        y += 24;
        return moved;
    }
};

} /* namespace */

/* --- one track's layout --- */
static void inspect_layout(App &a, Lane &L)
{
    Track *t = a.proj.track(a.layoutTrack);
    if (!t) {
        L.note("select a picture on the preview");
        return;
    }

    L.title(t->name.c_str());
    L.note("or drag it on the preview");
    L.gap(6);

    /* The switches first: they change what the numbers under them mean. */
    Rectangle lock = {L.x, L.y, L.w, 22};
    if (sn_toggle(&a.ui, 8710, lock, t->stretch ? "FREE" : "ASPECT LOCKED",
                  !t->stretch)) {
        t->stretch = !t->stretch;
        if (!t->stretch) t->scaleY = t->scaleX;
        a.changed();
    }
    L.y += 26;

    Rectangle fh = {L.x, L.y, L.w * 0.5f - 3, 22};
    Rectangle fv = {L.x + L.w * 0.5f + 3, L.y, L.w * 0.5f - 3, 22};
    if (sn_toggle(&a.ui, 8711, fh, "MIRROR L-R", t->flipH)) { t->flipH = !t->flipH; a.changed(); }
    if (sn_toggle(&a.ui, 8712, fv, "MIRROR U-D", t->flipV)) { t->flipV = !t->flipV; a.changed(); }
    L.y += 30;

    if (!t->stretch) {
        if (L.number(8720, "SIZE", &t->scaleX, 0.02, 4.0, "%.2f")) {
            t->scaleY = t->scaleX;
            a.changed(true);
        }
        L.note("1 fills the canvas");
    } else {
        if (L.number(8721, "WIDTH", &t->scaleX, 0.02, 4.0, "%.2f")) a.changed(true);
        if (L.number(8722, "HEIGHT", &t->scaleY, 0.02, 4.0, "%.2f")) a.changed(true);
    }
    L.gap(4);

    if (L.number(8723, "LEFT / RIGHT", &t->x, -2.0, 2.0, "%+.2f")) a.changed(true);
    L.note("-1 left edge, +1 right edge");
    if (L.number(8724, "UP / DOWN", &t->y, -2.0, 2.0, "%+.2f")) a.changed(true);
    L.note("-1 top, +1 bottom");
    L.gap(6);

    L.label("CROP");
    {
        double *crops[4] = {&t->cropL, &t->cropR, &t->cropT, &t->cropB};
        static const char *names[4] = {"L", "R", "T", "B"};
        const float cw = (L.w - 24) / 4.0f;
        for (int i = 0; i < 4; i++) {
            Rectangle sl = {L.x + i * (cw + 8), L.y, cw, 14};
            float v = (float)*crops[i];
            if (sn_slider(&a.ui, 8730 + i, sl, &v)) {
                const double other = *crops[i ^ 1];
                *crops[i] = std::max(0.0, std::min((double)v, 0.9 - other));
                a.changed(true);
            }
            char num[16];
            snprintf(num, sizeof num, "%s %.2f", names[i], *crops[i]);
            sn_text(&a.ui, SN_F_TINY, num, sl.x, L.y + 16, SN_EDGE);
        }
        L.y += 32;
    }
    L.gap(4);

    L.label("OR JUST");
    {
        struct Preset { const char *name; double scale, x, y; };
        static const Preset presets[4] = {{"FULL", 1.0, 0.0, 0.0},
                                          {"LEFT HALF", 0.5, -1.0, 0.0},
                                          {"RIGHT HALF", 0.5, 1.0, 0.0},
                                          {"CORNER", 0.32, 1.0, -1.0}};
        for (int i = 0; i < 4; i++) {
            Rectangle b = {L.x + (i % 2) * (L.w * 0.5f + 3), L.y + (i / 2) * 26.0f,
                           L.w * 0.5f - 3, 22};
            if (sn_button(&a.ui, 8740 + i, b, presets[i].name, 1)) {
                t->scaleX = t->scaleY = presets[i].scale;
                t->stretch = false;
                t->x = presets[i].x;
                t->y = presets[i].y;
                a.changed();
            }
        }
        L.y += 56;
    }

    Rectangle reset = {L.x, L.y, L.w, 24};
    if (sn_button(&a.ui, 8750, reset, "RESET THE LOT", t->transformed())) {
        t->resetTransform();
        a.changed();
    }
    L.y += 28;
}

/* --- one caption --- */
static void inspect_text(App &a, Lane &L)
{
    Clip *c = a.proj.clip(a.textClip);
    const Track *tr = a.proj.track(a.textClip.track);
    if (!c || !tr || tr->kind != TRACK_TEXT) {
        L.note("select a caption on the preview or the timeline");
        return;
    }
    TextStyle &st = c->text;

    L.title("CAPTION");
    L.note("drag it on the preview to move it");
    L.gap(6);

    /* --- the words --- */
    {
        std::string line[3];
        split_lines(st.text, line);

        L.label("WORDS");
        bool edited = false;
        for (int i = 0; i < 3; i++) {
            Rectangle f = {L.x, L.y + i * 24.0f, L.w, 21};
            if (sn_field(&a.ui, 8800 + i, f, line[i], i == 0 ? "the caption" : ""))
                edited = true;
        }
        if (edited) {
            st.text = join_lines(line);
            a.changed(true);
        }
        L.y += 24 * 3 + 4;
    }

    /* --- the face --- */
    {
        L.label("FONT");

        const std::vector<FontEntry> &fonts = systemFonts();
        std::string shown = st.font.empty() ? std::string("the one in the program")
                                            : std::string();
        if (shown.empty()) {
            for (const FontEntry &f : fonts)
                if (f.path == st.font) { shown = f.name; break; }
            if (shown.empty()) shown = st.font;
        }
        Color nameCol = SN_TEXT;
        if (textMissingFont(st)) {
            nameCol = SN_AMBER;
            shown += " - not on this machine";
        }
        sn_text_clip(&a.ui, SN_F_TINY, shown.c_str(), L.x, L.y, L.w, nameCol);
        L.y += 16;

        Rectangle filter = {L.x, L.y, L.w, 21};
        sn_field(&a.ui, 8811, filter, g_fontFilter, "type to find a font");
        L.y += 25;

        std::vector<const FontEntry *> hit;
        for (const FontEntry &f : fonts) {
            if (g_fontFilter.empty()) { hit.push_back(&f); continue; }
            std::string hay = f.name, ned = g_fontFilter;
            for (char &ch : hay) ch = (char)tolower((unsigned char)ch);
            for (char &ch : ned) ch = (char)tolower((unsigned char)ch);
            if (hay.find(ned) != std::string::npos) hit.push_back(&f);
        }

        const int rows = 5;
        Rectangle list = {L.x, L.y, L.w, rows * 18.0f};
        DrawRectangleRec(list, SN_WELL);
        DrawRectangleLinesEx(list, 1, SN_BORDER);

        const int maxTop = std::max(0, (int)hit.size() - rows);
        if (CheckCollisionPointRec(GetMousePosition(), list) && !sn_ui_blocked(&a.ui))
            g_fontScroll -= GetMouseWheelMove() * 2.0f;
        g_fontScroll = std::max(0.0f, std::min((float)maxTop, g_fontScroll));

        const int top = (int)g_fontScroll;
        for (int i = 0; i < rows && top + i < (int)hit.size(); i++) {
            const FontEntry *f = hit[(size_t)(top + i)];
            Rectangle row = {list.x + 1, list.y + 1 + i * 18.0f, list.width - 2, 17};
            const bool over = CheckCollisionPointRec(GetMousePosition(), row) &&
                              !sn_ui_blocked(&a.ui);
            const bool mine = f->path == st.font;
            if (mine) DrawRectangleRec(row, Color{0x2d, 0x5c, 0x8c, 140});
            else if (over) DrawRectangleRec(row, SN_PANEL);
            sn_text_clip(&a.ui, SN_F_TINY, f->name.c_str(), row.x + 5, row.y + 3,
                         row.width - 10, mine ? SN_TEXT : SN_DIM);
            if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                st.font = f->path;
                a.changed();
            }
        }
        if (hit.empty())
            sn_text(&a.ui, SN_F_TINY, "nothing matches", list.x + 5, list.y + 5, SN_EDGE);
        L.y += list.height + 6;

        char n[48];
        snprintf(n, sizeof n, "%d of %d", (int)hit.size(), (int)fonts.size());
        sn_text(&a.ui, SN_F_TINY, n, L.x, L.y + 4, SN_EDGE);

        Rectangle emb = {L.x + L.w - 74, L.y, 74, 20};
        if (sn_button(&a.ui, 8810, emb, "BUILT IN", !st.font.empty())) {
            st.font.clear();
            a.changed();
        }
        L.y += 24;
    }
    L.gap(4);

    if (L.number(8820, "SIZE", &st.size, 0.005, 2.0, "%.3f")) a.changed(true);
    if (L.number(8821, "TURN", &st.rotation, -180.0, 180.0, "%.0f")) {
        if (std::fabs(st.rotation) < 2.0) st.rotation = 0.0;
        a.changed(true);
    }
    if (L.number(8822, "OUTLINE", &st.outlineWidth, 0.0, 1.0, "%.3f")) a.changed(true);
    if (L.number(8823, "LINE GAP", &st.lineSpacing, 0.2, 6.0, "%.2f")) a.changed(true);
    L.gap(4);

    L.label("ALIGN");
    {
        static const char *names[3] = {"LEFT", "CENTRE", "RIGHT"};
        const float bw = (L.w - 8) / 3.0f;
        for (int i = 0; i < 3; i++) {
            Rectangle b = {L.x + i * (bw + 4), L.y, bw, 22};
            if (sn_toggle(&a.ui, 8830 + i, b, names[i], st.align == i)) {
                st.align = i;
                a.changed();
            }
        }
        L.y += 28;
    }

    if (colour_row(a, 8840, "FILL", &st.fill, L.x, L.y, L.w)) a.changed(true);
    L.y += 52;
    if (colour_row(a, 8850, "OUTLINE", &st.outline, L.x, L.y, L.w)) a.changed(true);
    L.y += 52;
}

void inspectPane(App &a, Rectangle r)
{
    sn_ui &ui = a.ui;
    if (r.width < 2) return;

    DrawRectangleRec(r, SN_PANEL);
    DrawLine((int)r.x, (int)r.y, (int)r.x, (int)(r.y + r.height), SN_BORDER);

    /* --- the header: which page, the pin, and the way out --- */
    Rectangle head = {r.x, r.y, r.width, 26};
    DrawRectangleRec(head, SN_PANEL_HI);
    sn_divider(head.x, head.y + head.height, head.width);

    {
        const float bw = (r.width - 70) * 0.5f;
        Rectangle lt = {r.x + 6, head.y + 3, bw, 20};
        Rectangle tt = {r.x + 10 + bw, head.y + 3, bw, 20};
        if (sn_toggle(&ui, 8690, lt, "PICTURE", a.inspectPage == App::INSPECT_LAYOUT))
            a.inspectPage = App::INSPECT_LAYOUT;
        if (sn_toggle(&ui, 8691, tt, "CAPTION", a.inspectPage == App::INSPECT_TEXT))
            a.inspectPage = App::INSPECT_TEXT;

        Rectangle pin = {r.x + r.width - 52, head.y + 3, 22, 20};
        if (sn_icon_button(&ui, 8692, pin, a.inspect.pinned ? SN_I_LOCK : SN_I_UNLOCK, 1,
                           a.inspect.pinned,
                           a.inspect.pinned ? "unpin it - it will close itself again"
                                            : "pin it open"))
            a.inspect.pinned = !a.inspect.pinned;

        Rectangle shut = {r.x + r.width - 28, head.y + 3, 22, 20};
        if (sn_icon_button(&ui, 8693, shut, SN_I_X, 1, 0, "close it")) {
            a.inspect.open = false;
            a.inspect.pinned = false;
        }
    }

    /* --- the body, scrolled --- */
    Rectangle body = {r.x, head.y + head.height + 1, r.width,
                      r.height - head.height - 1};

    BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);
    Lane L{&a, body.x + 10, body.width - 20, body.y + 8 - a.inspectScroll};
    const float top = L.y;

    if (a.inspectPage == App::INSPECT_TEXT) inspect_text(a, L);
    else inspect_layout(a, L);

    EndScissorMode();

    const float want = L.y - top + 16;
    if (CheckCollisionPointRec(GetMousePosition(), body) && !sn_ui_blocked(&ui))
        a.inspectScroll -= GetMouseWheelMove() * 30.0f;
    a.inspectScroll =
        std::max(0.0f, std::min(a.inspectScroll, std::max(0.0f, want - body.height)));

    /* A bar down the edge when there is more than fits, because a column that
     * scrolls and never says so is a column somebody stops at the bottom of. */
    if (want > body.height) {
        const float f = body.height / want;
        const float bh = std::max(20.0f, body.height * f);
        const float by = body.y + (body.height - bh) *
                                      (a.inspectScroll / std::max(1.0f, want - body.height));
        DrawRectangle((int)(r.x + r.width - 4), (int)by, 3, (int)bh, SN_EDGE);
    }
}


void canvasDialog(App &a)
{
    sn_ui &ui = a.ui;
    Rectangle r = modal_frame(a, "CANVAS", 520, 300);

    static std::string wTxt, hTxt, fTxt;
    static bool primed = false;
    if (!primed) {
        wTxt = std::to_string(a.proj.width);
        hTxt = std::to_string(a.proj.height);
        char f[32];
        snprintf(f, sizeof f, "%.3f", a.proj.fps);
        fTxt = f;
        primed = true;
    }

    float y = r.y + 46;
    label(a, "WIDTH", r.x + 16, y);
    label(a, "HEIGHT", r.x + 150, y);
    label(a, "FRAMES PER SECOND", r.x + 284, y);
    y += 14;

    sn_field(&ui, 8760, Rectangle{r.x + 16, y, 120, 24}, wTxt, "1920");
    sn_field(&ui, 8761, Rectangle{r.x + 150, y, 120, 24}, hTxt, "1080");
    sn_field(&ui, 8762, Rectangle{r.x + 284, y, 120, 24}, fTxt, "30");
    y += 40;

    label(a, "OR", r.x + 16, y);
    y += 14;

    struct Shape {
        const char *name;
        int w, h;
    } shapes[] = {
        {"1080P", 1920, 1080}, {"720P", 1280, 720},   {"4K", 3840, 2160},
        {"VERTICAL", 1080, 1920}, {"SQUARE", 1080, 1080},
    };
    for (int i = 0; i < 5; i++) {
        Rectangle b = {r.x + 16 + (i % 5) * 96.0f, y, 90, 24};
        const bool on = a.proj.width == shapes[i].w && a.proj.height == shapes[i].h;
        if (sn_toggle(&ui, 8770 + i, b, shapes[i].name, on)) {
            wTxt = std::to_string(shapes[i].w);
            hTxt = std::to_string(shapes[i].h);
        }
    }
    y += 40;

    /* What it is now, next to what it would become - the numbers on their own
     * do not say whether this is a change worth making. */
    {
        char now[128];
        snprintf(now, sizeof now, "now %dx%d at %.3f fps", a.proj.width, a.proj.height,
                 a.proj.fps);
        sn_text(&ui, SN_F_TINY, now, r.x + 16, y, SN_DIM);
        sn_text(&ui, SN_F_TINY,
                "tracks keep their layout: a track on the left half stays on the left "
                "half.",
                r.x + 16, y + 16, SN_EDGE);
    }

    Rectangle ok = {r.x + r.width - 200, r.y + r.height - 40, 88, 26};
    Rectangle no = {r.x + r.width - 104, r.y + r.height - 40, 88, 26};

    if (sn_button_lit(&ui, 8780, ok, "APPLY", 1)) {
        const int w = atoi(wTxt.c_str());
        const int h = atoi(hTxt.c_str());
        const double f = atof(fTxt.c_str());

        /* Even, because every encoder that matters refuses odd dimensions -
         * and refusing them here is a sentence, where refusing them at export
         * time is a failure ten seconds after choosing a filename. */
        if (w >= 16 && h >= 16 && w <= 16384 && h <= 16384 && f >= 1 && f <= 240) {
            a.proj.width = w & ~1;
            a.proj.height = h & ~1;
            a.proj.fps = f;
            a.changed();
            a.say("canvas is %dx%d at %.3f fps", a.proj.width, a.proj.height, a.proj.fps);
            primed = false;
            a.modal = MODAL_NONE;
        } else {
            a.complain("that is not a size this can use - 16 to 16384, 1 to 240 fps");
        }
    }
    if (sn_button(&ui, 8781, no, "CANCEL", 1) || IsKeyPressed(KEY_ESCAPE)) {
        primed = false;
        a.modal = MODAL_NONE;
    }
}

/* ------------------------------------------------------------------ *
 * Information
 * ------------------------------------------------------------------ */

void infoWindow(App &a)
{
    sn_ui &ui = a.ui;
    Rectangle r = modal_frame(a, "ABOUT", 620, 460);

    tarr(a, Vector2{r.x + 70, r.y + 90}, 38, TARR_IDLE);

    sn_text_spaced(&ui, SN_F_TITLE, SN_NAME, r.x + 130, r.y + 52, SN_TEXT);
    sn_text(&ui, SN_F_SMALL, "version " SN_VERSION, r.x + 130, r.y + 86, SN_DIM);
    sn_text(&ui, SN_F_TINY, "a video editor that does the small jobs without ceremony",
            r.x + 130, r.y + 106, SN_EDGE);

    float y = r.y + 140;
    sn_divider(r.x + 16, y, r.width - 32);
    y += 10;

    static int page = 0;
    static const char *tabs[] = {"BUILD", "LICENCE", "FONT", "NOTICE"};
    for (int i = 0; i < 4; i++) {
        Rectangle b = {r.x + 16 + i * 78.0f, y, 72, 22};
        if (sn_toggle(&ui, 8800 + i, b, tabs[i], page == i)) page = i;
    }
    y += 30;

    Rectangle body = {r.x + 16, y, r.width - 32, r.height - (y - r.y) - 56};
    sn_panel(body, SN_WELL, SN_BORDER);

    BeginScissorMode((int)body.x + 4, (int)body.y + 4, (int)body.width - 8,
                     (int)body.height - 8);

    if (page == 0) {
        char line[256];
        float ly = body.y + 8;
        auto row = [&](const char *k, const char *v) {
            sn_text(&ui, SN_F_TINY, k, body.x + 10, ly, SN_DIM);
            sn_text(&ui, SN_F_TINY, v, body.x + 120, ly, SN_TEXT);
            ly += 16;
        };
        row("version", SN_VERSION);
        row("ffmpeg", libVersion().c_str());
        row("raylib", RAYLIB_VERSION);
        row("graphics", glRenderer());
        snprintf(line, sizeof line, "%d x %d", GetScreenWidth(), GetScreenHeight());
        row("window", line);
        snprintf(line, sizeof line, "%d Hz, %d channels", (int)RATE, (int)CHANS);
        row("audio", line);
        row("h.264 encoder", haveEncoder("libx264") ? "libx264" : "not in this build");
        row("vp9 encoder", haveEncoder("libvpx-vp9") ? "libvpx-vp9" : "not in this build");
        row("aac encoder", haveEncoder("aac") ? "aac" : "not in this build");

        /* How long this copy took to start.
         *
         * Two lines rather than the whole breakdown, which is what --timing
         * prints; these are the two numbers that decide where to look. If the
         * window took a moment and the rest was quick, the time went into the
         * driver or the loader and none of it is this program's to give back.
         * If they are far apart, it is ours.
         *
         * Shown here at all because the Windows build is linked for the
         * windowing subsystem and has nowhere to print, and "it takes fifteen
         * seconds to open" needs a number attached before anyone can act on
         * it. */
        snprintf(line, sizeof line, "%.0f ms to the window, %.0f ms to ready",
                 startupWindowMs(), startupReadyMs());
        row("startup", line);

        /* Where the drawers' state is kept. One line, because "it opened with
         * the panel shut and I do not know why" has one answer and it is a
         * file somebody can look at or delete. */
        {
            const std::string sp = prefsPath();
            row("settings", sp.empty() ? "nowhere - this machine will not say"
                                       : sp.c_str());
        }

        ly += 8;
        sn_text(&ui, SN_F_TINY, "S. Tarr is BENCO's audio/visual person. He is drawn,",
                body.x + 10, ly, SN_EDGE);
        ly += 14;
        sn_text(&ui, SN_F_TINY, "not loaded, which is why he is never missing.",
                body.x + 10, ly, SN_EDGE);
    } else {
        const char *text = page == 1   ? (const char *)SN_LICENSE_MIT
                           : page == 2 ? (const char *)SN_LICENSE_OFL
                                       : (const char *)SN_NOTICE;

        static float scroll[4] = {0, 0, 0, 0};
        if (CheckCollisionPointRec(GetMousePosition(), body))
            scroll[page] -= GetMouseWheelMove() * 30;
        if (scroll[page] < 0) scroll[page] = 0;

        float ly = body.y + 8 - scroll[page];
        const char *p = text;
        char line[256];
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);

            /* The OFL ships with CRLF endings, and the carriage return is a
             * codepoint the font has no glyph for - so every line of it drew
             * with a question mark on the end. */
            if (n && p[n - 1] == '\r') n--;

            if (n >= sizeof line) {
                n = sizeof line - 1;
                /* Never cut through a UTF-8 sequence: the tail bytes of one
                 * are not characters, and drawing them is the same question
                 * mark by a different route. */
                while (n && (p[n] & 0xC0) == 0x80) n--;
            }
            memcpy(line, p, n);
            line[n] = 0;
            if (ly > body.y - 14 && ly < body.y + body.height)
                sn_text(&ui, SN_F_TINY, line, body.x + 10, ly, SN_DIM);
            ly += 13;
            if (!nl) break;
            p = nl + 1;
        }
    }
    EndScissorMode();

    Rectangle close = {r.x + r.width - 104, r.y + r.height - 40, 88, 26};
    if (sn_button(&ui, 8810, close, "CLOSE", 1) || IsKeyPressed(KEY_ESCAPE))
        a.modal = MODAL_NONE;
}

/* ------------------------------------------------------------------ *
 * Confirmation
 * ------------------------------------------------------------------ */

void confirmDialog(App &a)
{
    sn_ui &ui = a.ui;
    Rectangle r = modal_frame(a, "HOLD ON", 460, 170);

    sn_text_center(&ui, SN_F_SMALL, a.confirmText.c_str(), r.x + r.width * 0.5f, r.y + 60,
                   SN_TEXT);

    Rectangle yes = {r.x + r.width * 0.5f - 100, r.y + r.height - 48, 92, 26};
    Rectangle no = {r.x + r.width * 0.5f + 8, r.y + r.height - 48, 92, 26};

    if (sn_button_lit(&ui, 8900, yes, "YES", 1) || IsKeyPressed(KEY_ENTER)) {
        a.modal = MODAL_NONE;
        if (a.confirmTag == 1) a.quit = true;
        else if (a.confirmTag == 2) {
            a.proj = newProject();
            a.hist.reset(a.proj);
            a.thumbs.clear();
            a.playhead = 0;
            a.changed(true);
            a.say("new project");
        }
    }
    if (sn_button(&ui, 8901, no, "NO", 1) || IsKeyPressed(KEY_ESCAPE)) a.modal = MODAL_NONE;
}

} /* namespace sn */
