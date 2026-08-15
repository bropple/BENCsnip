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
#include "sn_embed.h"
#include "sn_filedlg.h"
#include "sn_version.h"

#include <algorithm>
#include <cmath>
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

bool colour_row(App &a, int id, const char *name, Rgba *c, float x, float y, float w)
{
    sn_ui &ui = a.ui;
    label(a, name, x, y + 4);

    bool changed = false;

    const unsigned r = (*c >> 24) & 0xff, g = (*c >> 16) & 0xff;
    const unsigned b = (*c >> 8) & 0xff, al = *c & 0xff;

    /* The swatch, over a chequer so an alpha of nothing looks like nothing
     * rather than like black. */
    Rectangle sw = {x + 98, y, 30, 22};
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

    Rectangle hf = {x + 134, y, 96, 22};
    if (sn_field(&ui, id, hf, text, "RRGGBB")) {
        Rgba n = *c;
        if (hex_to(text, &n) && n != *c) { *c = n; changed = true; }
    }

    Rectangle pick = {x + 238, y, 74, 22};
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

    label(a, "ALPHA", x + 326, y + 4);
    Rectangle as = {x + 386, y + 4, w - 386 - 52, 14};
    float av = al / 255.0f;
    if (sn_slider(&ui, id + 2, as, &av)) {
        *c = (*c & 0xffffff00u) | (Rgba)(unsigned)(av * 255.0f + 0.5f);
        changed = true;
    }
    {
        char n[16];
        snprintf(n, sizeof n, "%d%%", (int)(al * 100 / 255));
        sn_text(&ui, SN_F_TINY, n, as.x + as.width + 8, y + 4, SN_EDGE);
    }

    return changed;
}

/* The font list, filtered. Kept between frames because it is the dialog's
 * own state and not the project's - closing the window forgets it, which is
 * what you want from a search box. */
std::string g_fontFilter;
float g_fontScroll = 0;

} /* namespace */

void textDialog(App &a)
{
    sn_ui &ui = a.ui;

    Clip *c = a.proj.clip(a.textClip);
    const Track *tr = a.proj.track(a.textClip.track);
    if (!c || !tr || tr->kind != TRACK_TEXT) { a.modal = MODAL_NONE; return; }

    TextStyle &st = c->text;

    Rectangle r = modal_frame(a, "CAPTION", 620, 590);

    sn_text(&ui, SN_F_TINY,
            "drag it about on the preview behind this, or use its handles to resize "
            "and turn it.",
            r.x + 16, r.y + 38, SN_DIM);

    float y = r.y + 62;

    /* --- the words --- */
    {
        std::string line[3];
        split_lines(st.text, line);

        label(a, "WORDS", r.x + 16, y + 4);
        bool edited = false;
        for (int i = 0; i < 3; i++) {
            Rectangle f = {r.x + 114, y + i * 26.0f, 480, 22};
            if (sn_field(&ui, 8800 + i, f, line[i], i == 0 ? "the caption" : ""))
                edited = true;
        }
        if (edited) {
            st.text = join_lines(line);
            a.changed(true);
        }
        y += 26 * 3 + 12;
    }

    /* --- the face --- */
    {
        label(a, "FONT", r.x + 16, y + 4);

        const std::vector<FontEntry> &fonts = systemFonts();

        std::string shown = st.font.empty() ? std::string("the one in the program")
                                            : std::string();
        if (shown.empty()) {
            for (const FontEntry &f : fonts)
                if (f.path == st.font) { shown = f.name; break; }
            if (shown.empty()) shown = st.font;    /* a path nothing here lists */
        }

        Color nameCol = SN_TEXT;
        if (textMissingFont(st)) {
            nameCol = SN_AMBER;
            shown += "  - not on this machine, using the program's";
        }
        sn_text_clip(&ui, SN_F_SMALL, shown.c_str(), r.x + 114, y, 380, nameCol);

        Rectangle emb = {r.x + 506, y - 4, 88, 22};
        if (sn_button(&ui, 8810, emb, "BUILT IN", !st.font.empty())) {
            st.font.clear();
            a.changed();
        }
        y += 26;

        Rectangle filter = {r.x + 114, y, 480, 22};
        sn_field(&ui, 8811, filter, g_fontFilter, "type to find a font");
        y += 28;

        /* The matches. Six rows: enough to choose from, small enough to leave
         * the colours on the same screen. */
        std::vector<const FontEntry *> hit;
        for (const FontEntry &f : fonts) {
            if (g_fontFilter.empty()) { hit.push_back(&f); continue; }
            std::string hay = f.name, ned = g_fontFilter;
            for (char &ch : hay) ch = (char)tolower((unsigned char)ch);
            for (char &ch : ned) ch = (char)tolower((unsigned char)ch);
            if (hay.find(ned) != std::string::npos) hit.push_back(&f);
        }

        const int rows = 6;
        Rectangle list = {r.x + 114, y, 480, rows * 20.0f};
        DrawRectangleRec(list, SN_WELL);
        DrawRectangleLinesEx(list, 1, SN_BORDER);

        const int maxTop = std::max(0, (int)hit.size() - rows);
        if (CheckCollisionPointRec(GetMousePosition(), list) && !sn_ui_blocked(&ui))
            g_fontScroll -= GetMouseWheelMove() * 2.0f;
        g_fontScroll = std::max(0.0f, std::min((float)maxTop, g_fontScroll));

        const int top = (int)g_fontScroll;
        for (int i = 0; i < rows && top + i < (int)hit.size(); i++) {
            const FontEntry *f = hit[(size_t)(top + i)];
            Rectangle row = {list.x + 1, list.y + 1 + i * 20.0f, list.width - 2, 19};
            const bool over = CheckCollisionPointRec(GetMousePosition(), row) &&
                              !sn_ui_blocked(&ui);
            const bool mine = f->path == st.font;

            if (mine) DrawRectangleRec(row, Color{0x2d, 0x5c, 0x8c, 140});
            else if (over) DrawRectangleRec(row, SN_PANEL);

            sn_text_clip(&ui, SN_F_TINY, f->name.c_str(), row.x + 6, row.y + 4,
                         row.width - 12, mine ? SN_TEXT : SN_DIM);

            if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                st.font = f->path;
                a.changed();
            }
        }
        if (hit.empty())
            sn_text(&ui, SN_F_TINY, "nothing on this machine matches", list.x + 6,
                    list.y + 6, SN_EDGE);

        {
            char n[64];
            snprintf(n, sizeof n, "%d of %d", (int)hit.size(), (int)fonts.size());
            sn_text(&ui, SN_F_TINY, n, r.x + 16, list.y + 4, SN_EDGE);
        }
        y += list.height + 14;
    }

    /* --- size, turn, spacing --- */
    struct Num {
        const char *name;
        double *v, lo, hi;
        const char *fmt;
    } nums[] = {
        {"SIZE", &st.size, 0.01, 0.60, "%.3f of the height"},
        {"TURN", &st.rotation, -180.0, 180.0, "%.1f degrees"},
        {"OUTLINE", &st.outlineWidth, 0.0, 0.35, "%.3f of the size"},
        {"LINE GAP", &st.lineSpacing, 0.8, 2.5, "%.2f x the line"},
    };
    for (int i = 0; i < 4; i++) {
        label(a, nums[i].name, r.x + 16, y);
        Rectangle sl = {r.x + 114, y - 4, 340, 16};
        float v = (float)((*nums[i].v - nums[i].lo) / (nums[i].hi - nums[i].lo));
        if (sn_slider(&ui, 8820 + i, sl, &v)) {
            *nums[i].v = nums[i].lo + v * (nums[i].hi - nums[i].lo);
            /* Upright is sticky, the same way it is on the preview's turn
             * handle: level is what nearly every caption wants. */
            if (i == 1 && std::fabs(*nums[i].v) < 2.0) *nums[i].v = 0.0;
            a.changed(true);
        }
        char num[64];
        snprintf(num, sizeof num, nums[i].fmt, *nums[i].v);
        sn_text(&ui, SN_F_TINY, num, sl.x + sl.width + 12, y - 2, SN_EDGE);
        y += 26;
    }
    y += 6;

    /* --- alignment, which only shows with more than one line --- */
    {
        label(a, "ALIGN", r.x + 16, y);
        static const char *names[3] = {"LEFT", "CENTRE", "RIGHT"};
        for (int i = 0; i < 3; i++) {
            Rectangle b = {r.x + 114 + i * 96.0f, y - 6, 90, 22};
            if (sn_toggle(&ui, 8830 + i, b, names[i], st.align == i)) {
                st.align = i;
                a.changed();
            }
        }
        if (st.text.find('\n') == std::string::npos)
            sn_text(&ui, SN_F_TINY, "needs more than one line", r.x + 410, y - 2,
                    SN_EDGE);
        y += 30;
    }

    /* --- the two colours --- */
    if (colour_row(a, 8840, "FILL", &st.fill, r.x + 16, y, 580)) a.changed(true);
    y += 30;
    if (colour_row(a, 8850, "OUTLINE", &st.outline, r.x + 16, y, 580)) a.changed(true);
    y += 30;

    Rectangle close = {r.x + r.width - 104, r.y + r.height - 40, 88, 26};
    if (sn_button_lit(&ui, 8860, close, "DONE", 1) || IsKeyPressed(KEY_ESCAPE)) {
        a.changed();
        a.modal = MODAL_NONE;
    }
}

void layoutDialog(App &a)
{
    sn_ui &ui = a.ui;

    Track *t = a.proj.track(a.layoutTrack);
    if (!t) { a.modal = MODAL_NONE; return; }

    Rectangle r = modal_frame(a, "TRACK LAYOUT", 560, 520);

    sn_text_spaced(&ui, SN_F_SMALL, t->name.c_str(), r.x + 16, r.y + 38, SN_ACCENT);
    sn_text(&ui, SN_F_TINY,
            "or just drag it about on the preview. The export uses the same numbers.",
            r.x + 56, r.y + 40, SN_DIM);

    /* --- the picture of it --- */
    const double car = a.proj.height > 0 ? (double)a.proj.width / a.proj.height : 16.0 / 9.0;
    float pw = 280, ph = (float)(280 / car);
    if (ph > 170) { ph = 170; pw = (float)(170 * car); }

    Rectangle canvas = {r.x + (560 - pw) * 0.5f, r.y + 66, pw, ph};
    DrawRectangleRec(canvas, SN_BG);
    DrawRectangleLinesEx(canvas, 1, SN_BORDER);

    /* The layer, worked out the same way the renderer works it out - crop
     * first, because it changes the shape that gets fitted. */
    {
        const double keepX = std::max(0.02, 1.0 - t->cropL - t->cropR);
        const double keepY = std::max(0.02, 1.0 - t->cropT - t->cropB);

        double sw = 16, sh = 9;
        for (const Clip &c : t->clips) {
            const BinItem *b = a.proj.item(c.source);
            if (b && b->info.hasVideo) { sw = b->info.dispW(); sh = b->info.dispH(); break; }
        }
        sw *= keepX;
        sh *= keepY;

        const double boxW = pw * std::max(0.01, t->scaleX);
        const double boxH = ph * std::max(0.01, t->scaleY);
        const double sa = sw / sh, ba = boxW / boxH;

        double lw = t->stretch ? boxW : (sa > ba ? boxW : boxH * sa);
        double lh = t->stretch ? boxH : (sa > ba ? boxW / sa : boxH);

        const double ox = (pw - lw) * 0.5 * (1.0 + t->x);
        const double oy = (ph - lh) * 0.5 * (1.0 + t->y);

        Rectangle layer = {canvas.x + (float)ox, canvas.y + (float)oy, (float)lw,
                           (float)lh};
        DrawRectangleRec(layer, Color{0x2d, 0x5c, 0x8c, 200});
        DrawRectangleLinesEx(layer, 1, SN_ACCENT);
        sn_text_center(&ui, SN_F_TINY, t->name.c_str(), layer.x + layer.width * 0.5f,
                       layer.y + layer.height * 0.5f - 6, SN_TEXT);
    }

    /* --- the numbers --- */
    float y = canvas.y + ph + 16;

    /* The lock decides whether SIZE is one number or two. Locked is the
     * common case by a mile, so it is one slider until somebody says
     * otherwise - two sliders that always have to be dragged together are
     * two chances to get it slightly wrong. */
    {
        Rectangle lock = {r.x + 400, y - 6, 144, 22};
        if (sn_toggle(&ui, 8710, lock, t->stretch ? "FREE" : "ASPECT LOCKED",
                      !t->stretch)) {
            t->stretch = !t->stretch;
            if (!t->stretch) t->scaleY = t->scaleX;   /* back to square */
            a.changed();
        }
        if (CheckCollisionPointRec(GetMousePosition(), lock) && !sn_ui_blocked(&ui))
            sn_tip(&ui, t->stretch
                            ? "the picture fills the box and may distort"
                            : "the picture keeps its shape inside the box");
    }

    if (!t->stretch) {
        label(a, "SIZE", r.x + 16, y);
        Rectangle sl = {r.x + 130, y - 4, 260, 16};
        float v = (float)((t->scaleX - 0.05) / (2.0 - 0.05));
        if (sn_slider(&ui, 8720, sl, &v)) {
            t->scaleX = t->scaleY = 0.05 + v * (2.0 - 0.05);
            a.changed(true);
        }
        char num[32];
        snprintf(num, sizeof num, "%.2f", t->scaleX);
        sn_text(&ui, SN_F_TINY, num, sl.x + sl.width + 10, y - 2, SN_TEXT);
        sn_text(&ui, SN_F_TINY, "1 fills the canvas", r.x + 16, y + 12, SN_EDGE);
        y += 34;
    } else {
        const char *names[] = {"WIDTH", "HEIGHT"};
        double *vals[] = {&t->scaleX, &t->scaleY};
        for (int i = 0; i < 2; i++) {
            label(a, names[i], r.x + 16, y);
            Rectangle sl = {r.x + 130, y - 4, 260, 16};
            float v = (float)((*vals[i] - 0.05) / (2.0 - 0.05));
            if (sn_slider(&ui, 8722 + i, sl, &v)) {
                *vals[i] = 0.05 + v * (2.0 - 0.05);
                a.changed(true);
            }
            char num[32];
            snprintf(num, sizeof num, "%.2f", *vals[i]);
            sn_text(&ui, SN_F_TINY, num, sl.x + sl.width + 10, y - 2, SN_TEXT);
            y += 24;
        }
        sn_text(&ui, SN_F_TINY, "1 fills the canvas on that axis", r.x + 16, y - 10,
                SN_EDGE);
        y += 10;
    }

    {
        const char *names[] = {"LEFT / RIGHT", "UP / DOWN"};
        double *vals[] = {&t->x, &t->y};
        const char *hints[] = {"-1 left edge, +1 right edge", "-1 top, +1 bottom"};
        for (int i = 0; i < 2; i++) {
            label(a, names[i], r.x + 16, y);
            Rectangle sl = {r.x + 130, y - 4, 260, 16};
            float v = (float)((*vals[i] + 1.0) / 2.0);
            if (sn_slider(&ui, 8725 + i, sl, &v)) {
                *vals[i] = -1.0 + v * 2.0;
                a.changed(true);
            }
            char num[32];
            snprintf(num, sizeof num, "%+.2f", *vals[i]);
            sn_text(&ui, SN_F_TINY, num, sl.x + sl.width + 10, y - 2, SN_TEXT);
            sn_text(&ui, SN_F_TINY, hints[i], r.x + 16, y + 12, SN_EDGE);
            y += 34;
        }
    }

    /* Crop, as four numbers on one line: it is one idea, not four. */
    label(a, "CROP  L R T B", r.x + 16, y);
    double *crops[] = {&t->cropL, &t->cropR, &t->cropT, &t->cropB};
    for (int i = 0; i < 4; i++) {
        Rectangle sl = {r.x + 130 + i * 68.0f, y - 4, 56, 16};
        float v = (float)*crops[i];
        if (sn_slider(&ui, 8730 + i, sl, &v)) {
            /* Two opposite crops that meet would leave nothing to draw. */
            double other = (i < 2) ? *crops[i ^ 1] : *crops[(i ^ 1)];
            *crops[i] = std::min((double)v, 0.9 - other);
            if (*crops[i] < 0) *crops[i] = 0;
            a.changed(true);
        }
        char num[16];
        snprintf(num, sizeof num, "%.2f", *crops[i]);
        sn_text(&ui, SN_F_TINY, num, sl.x + 14, y + 12, SN_EDGE);
    }
    y += 40;

    /* --- the two layouts anybody actually wants --- */
    label(a, "OR JUST", r.x + 16, y);
    struct Preset {
        const char *name;
        double scale, x, y;
    } presets[] = {
        {"FULL", 1.0, 0.0, 0.0},   {"LEFT HALF", 0.5, -1.0, 0.0},
        {"RIGHT HALF", 0.5, 1.0, 0.0}, {"CORNER", 0.32, 1.0, -1.0},
    };
    for (int i = 0; i < 4; i++) {
        Rectangle b = {r.x + 130 + i * 96.0f, y - 6, 90, 22};
        if (sn_button(&ui, 8740 + i, b, presets[i].name, 1)) {
            t->scaleX = t->scaleY = presets[i].scale;
            t->stretch = false;
            t->x = presets[i].x;
            t->y = presets[i].y;
            a.changed();
        }
    }

    Rectangle reset = {r.x + 16, r.y + r.height - 40, 88, 26};
    Rectangle close = {r.x + r.width - 104, r.y + r.height - 40, 88, 26};
    if (sn_button(&ui, 8750, reset, "RESET", t->transformed())) {
        t->resetTransform();
        a.changed();
    }
    if (sn_button_lit(&ui, 8751, close, "DONE", 1) || IsKeyPressed(KEY_ESCAPE)) {
        a.changed();
        a.modal = MODAL_NONE;
    }
}

/* ------------------------------------------------------------------ *
 * The canvas
 *
 * What the preview shows and what an export defaults to. It is set from the
 * first video dropped in, which is right nearly always and wrong exactly when
 * someone wants a shape none of their footage is - two videos side by side in
 * a wide frame, or a phone-shaped crop out of landscape.
 * ------------------------------------------------------------------ */

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
