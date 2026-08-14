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
#include "sn_embed.h"
#include "sn_filedlg.h"
#include "sn_version.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
 * format with no encoder for it is a button that can only disappoint. */
bool usable(const Preset &p)
{
    if (p.video[0] && !*pick(p.video)) return false;
    return *pick(p.audio) != 0;
}

int g_preset = 0;
int g_res = 0;         /* 0 source, 1 1080p, 2 720p, 3 480p */
float g_quality = 0.6f;
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
    if (videoOut) {
        label(a, "SIZE", r.x + 16, y);
        y += 14;
        static const char *sizes[] = {"SOURCE", "1080P", "720P", "480P"};
        for (int i = 0; i < 4; i++) {
            Rectangle b = {r.x + 16 + i * 76.0f, y, 70, 24};
            if (sn_toggle(&ui, 8620 + i, b, sizes[i], g_res == i)) g_res = i;
        }
        y += 34;

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

    /* --- what it is going to do --- */
    a.ex.path = g_path;
    a.ex.vcodec = PRESETS[g_preset].video[0] ? pick(PRESETS[g_preset].video) : "";
    a.ex.acodec = pick(PRESETS[g_preset].audio);
    a.ex.fps = a.proj.fps;
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
        snprintf(line, sizeof line, "%d x %d", GetScreenWidth(), GetScreenHeight());
        row("window", line);
        snprintf(line, sizeof line, "%d Hz, %d channels", (int)RATE, (int)CHANS);
        row("audio", line);
        row("h.264 encoder", haveEncoder("libx264") ? "libx264" : "not in this build");
        row("vp9 encoder", haveEncoder("libvpx-vp9") ? "libvpx-vp9" : "not in this build");
        row("aac encoder", haveEncoder("aac") ? "aac" : "not in this build");
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
            if (n >= sizeof line) n = sizeof line - 1;
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
