/*
 * BENCsnip - the project file
 */

#include "sn_project.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace sn {

static const char *MAGIC = "bencsnip";
static const int FORMAT = 1;

static std::string dirname_of(const std::string &p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

static std::string basename_of(const std::string &p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

static bool is_absolute(const std::string &p)
{
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() > 2 && p[1] == ':';        /* C:\ */
}

static bool exists(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ *
 * Import
 * ------------------------------------------------------------------ */

int importFile(Project &p, const std::string &path, std::string *err)
{
    for (const BinItem &b : p.bin)
        if (b.info.path == path) return b.id;

    MediaInfo mi;
    if (!probe(path, &mi, err)) return 0;

    BinItem b;
    b.id = p.newId();
    b.info = mi;
    p.bin.push_back(b);

    /* The first video in decides what the project is, because the alternative
     * is asking, and nobody dropping a phone video into an editor wants a
     * dialog about resolution first. Changing it later is one field in the
     * export dialog. */
    static_cast<void>(0);
    bool firstVideo = mi.hasVideo;
    for (const BinItem &o : p.bin)
        if (o.id != b.id && o.info.hasVideo) firstVideo = false;
    if (firstVideo) {
        p.width = mi.dispW();
        p.height = mi.dispH();

        /* Its size, but not its frame rate if it is a photograph. A still has
         * no rate of its own; what libav reports for one is whatever the
         * image demuxer defaults to - 25 for a png - and a project set to 25
         * because somebody imported a title card first is a project that
         * quietly resamples every video that follows. */
        if (mi.fps > 0 && !mi.still) p.fps = mi.fps;
    }

    p.dirty = true;
    return b.id;
}

bool relink(Project &p, int itemId, const std::string &path, std::string *err)
{
    BinItem *b = p.item(itemId);
    if (!b) { if (err) *err = "no such bin item"; return false; }

    MediaInfo mi;
    if (!probe(path, &mi, err)) return false;

    b->info = mi;
    b->missing = false;
    p.dirty = true;
    return true;
}

/* ------------------------------------------------------------------ *
 * Save
 * ------------------------------------------------------------------ */

/* A caption on one line.
 *
 * The text of a clip is the only thing in this format that can hold a newline
 * or a run of spaces that has to survive, so it is the only thing escaped: a
 * backslash for a backslash, \n for a line break. Everything else in the file
 * is numbers and a name that runs to the end of its line.
 */
static std::string escape_text(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') continue;
        else out += ch;
    }
    return out;
}

static std::string unescape_text(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        const char n = s[++i];
        if (n == 'n') out += '\n';
        else if (n == '\\') out += '\\';
        else { out += '\\'; out += n; }
    }
    return out;
}

bool saveProject(const Project &p, const std::string &path, std::string *err)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "cannot write " + path; return false; }

    const std::string dir = dirname_of(path);

    fprintf(f, "%s %d\n", MAGIC, FORMAT);
    fprintf(f, "# %s project file. Paths are relative to this file where they can be.\n",
            "BENCsnip");
    fprintf(f, "name %s\n", p.name.c_str());
    fprintf(f, "video %d %d %.6f\n", p.width, p.height, p.fps);
    fprintf(f, "next %d\n", p.nextId);

    for (const BinItem &b : p.bin) {
        /* A project and its footage usually travel together, so a path
         * inside the project's own folder is stored relative: the folder can
         * then be moved or copied to another machine and still open. */
        std::string rel = b.info.path;
        if (rel.compare(0, dir.size(), dir) == 0 && rel.size() > dir.size() &&
            (rel[dir.size()] == '/' || rel[dir.size()] == '\\'))
            rel = rel.substr(dir.size() + 1);

        fprintf(f, "item %d %.6f %d %d %s\n", b.id, b.info.duration,
                b.info.hasVideo ? 1 : 0, b.info.hasAudio ? 1 : 0, rel.c_str());
    }

    for (const Track &t : p.tracks) {
        fprintf(f, "track %d %d %d %d %d %s\n", t.id, (int)t.kind, t.muted ? 1 : 0,
                t.locked ? 1 : 0, t.height, t.name.c_str());

        /* Its own line rather than more fields on the track line, and only
         * when there is something to say. A reader that predates this skips
         * a line it does not recognise; one that had to parse four more
         * numbers before the track's name would read the name as a number. */
        if (t.transformed())
            fprintf(f, "xform %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %d\n", t.scaleX,
                    t.x, t.y, t.cropL, t.cropR, t.cropT, t.cropB, t.scaleY,
                    t.stretch ? 1 : 0);

        /* Its own line for the same reason, and only when it is not unity, so
         * that a project nobody has touched the levels of is byte for byte the
         * file it was before this existed. */
        if (t.gain != 1.0) fprintf(f, "level %.6f\n", t.gain);
        for (const Clip &c : t.clips) {
            fprintf(f, "clip %d %d %d %.6f %.6f %.6f %.4f %.4f %.4f %d %.6f\n",
                    c.id, c.source, c.link, c.in, c.out, c.pos, c.gain,
                    c.fadeIn, c.fadeOut, c.muted ? 1 : 0, c.repeat);

            /* What a caption says and how it looks, on its own lines after
             * the clip they belong to - the arrangement `xform` and `level`
             * already use, and for the same reasons: an older reader skips a
             * line it does not know, and nothing had to be squeezed onto the
             * clip line in front of a field that is already there.
             *
             * Only on text tracks. Every clip carries a TextStyle because a
             * caption is a clip; writing an unused one under every piece of
             * footage would treble the size of a project file to say nothing. */
            if (t.kind != TRACK_TEXT) continue;

            fprintf(f, "tstyle %.6f %.6f %.6f %.6f %u %u %.6f %d %.6f\n",
                    c.text.size, c.text.x, c.text.y, c.text.rotation,
                    (unsigned)c.text.fill, (unsigned)c.text.outline,
                    c.text.outlineWidth, c.text.align, c.text.lineSpacing);
            if (!c.text.font.empty()) fprintf(f, "tfont %s\n", c.text.font.c_str());
            if (!c.text.text.empty())
                fprintf(f, "tstr %s\n", escape_text(c.text.text).c_str());
        }
    }

    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ *
 * Load
 * ------------------------------------------------------------------ */

bool loadProject(Project *out, const std::string &path, std::string *err)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "cannot open " + path; return false; }

    char line[4096];
    if (!fgets(line, sizeof line, f)) { fclose(f); if (err) *err = "empty file"; return false; }

    int ver = 0;
    char magic[32] = {0};
    if (sscanf(line, "%31s %d", magic, &ver) != 2 || strcmp(magic, MAGIC) != 0) {
        fclose(f);
        if (err) *err = basename_of(path) + " is not a BENCsnip project";
        return false;
    }
    if (ver > FORMAT) {
        fclose(f);
        if (err) *err = "project was written by a newer BENCsnip (format " +
                        std::to_string(ver) + ")";
        return false;
    }

    Project p;
    p.tracks.clear();
    p.path = path;
    p.name = basename_of(path);
    const std::string dir = dirname_of(path);

    int trackAt = -1;
    int clipAt = -1;   /* which clip the tstyle/tfont/tstr lines belong to */

    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (line[0] == '#' || line[0] == 0) continue;

        if (!strncmp(line, "name ", 5)) {
            p.name = line + 5;
        } else if (!strncmp(line, "video ", 6)) {
            sscanf(line + 6, "%d %d %lf", &p.width, &p.height, &p.fps);
        } else if (!strncmp(line, "next ", 5)) {
            p.nextId = atoi(line + 5);
        } else if (!strncmp(line, "item ", 5)) {
            int id = 0, hv = 0, ha = 0;
            double dur = 0;
            int used = 0;
            if (sscanf(line + 5, "%d %lf %d %d %n", &id, &dur, &hv, &ha, &used) < 4) continue;
            std::string rel = line + 5 + used;

            std::string full = is_absolute(rel) ? rel : dir + "/" + rel;
            if (!exists(full) && exists(rel)) full = rel;

            BinItem b;
            b.id = id;
            std::string perr;
            if (!probe(full, &b.info, &perr)) {
                /* Keep the edit. A missing file is a relink, not a lost
                 * afternoon. */
                b.missing = true;
                b.info.path = full;
                b.info.name = basename_of(full);
                b.info.duration = dur;
                b.info.hasVideo = hv != 0;
                b.info.hasAudio = ha != 0;
            }
            p.bin.push_back(b);
        } else if (!strncmp(line, "track ", 6)) {
            Track t;
            int kind = 0, muted = 0, locked = 0, used = 0;
            if (sscanf(line + 6, "%d %d %d %d %d %n", &t.id, &kind, &muted, &locked,
                       &t.height, &used) < 5) continue;
            t.kind = kind == TRACK_AUDIO ? TRACK_AUDIO
                     : kind == TRACK_TEXT ? TRACK_TEXT
                                          : TRACK_VIDEO;
            t.muted = muted != 0;
            t.locked = locked != 0;
            t.name = line + 6 + used;
            p.tracks.push_back(t);
            trackAt = (int)p.tracks.size() - 1;
            clipAt = -1;
        } else if (!strncmp(line, "xform ", 6)) {
            if (trackAt < 0) continue;
            Track &t = p.tracks[trackAt];
            int stretch = 0;
            /* The last two arrived later. A file written before they existed
             * has one scale for both axes and no stretching, which is what
             * these defaults say. */
            const int got = sscanf(line + 6, "%lf %lf %lf %lf %lf %lf %lf %lf %d",
                                   &t.scaleX, &t.x, &t.y, &t.cropL, &t.cropR, &t.cropT,
                                   &t.cropB, &t.scaleY, &stretch);
            if (got < 8) t.scaleY = t.scaleX;
            t.stretch = stretch != 0;
        } else if (!strncmp(line, "level ", 6)) {
            if (trackAt < 0) continue;
            double g = 1.0;
            /* Clamped to the range the fader offers. A hand-edited file
             * asking for forty is a mix nobody can find the way back from,
             * and the renderer would clip it to a square wave anyway. */
            if (sscanf(line + 6, "%lf", &g) == 1)
                p.tracks[trackAt].gain = g < 0.0 ? 0.0 : (g > 2.0 ? 2.0 : g);
        } else if (!strncmp(line, "clip ", 5)) {
            if (trackAt < 0) continue;
            Clip c;
            int muted = 0;
            /* The repeat count is the eleventh field and arrived later, so a
             * file without one is a file from before looping existed and
             * means once. */
            if (sscanf(line + 5, "%d %d %d %lf %lf %lf %lf %lf %lf %d %lf",
                       &c.id, &c.source, &c.link, &c.in, &c.out, &c.pos,
                       &c.gain, &c.fadeIn, &c.fadeOut, &muted, &c.repeat) < 10)
                continue;
            if (!(c.repeat > 0)) c.repeat = 1.0;
            c.muted = muted != 0;
            if (c.dur() > 0) {
                p.tracks[trackAt].clips.push_back(c);
                clipAt = (int)p.tracks[trackAt].clips.size() - 1;
            }
        } else if (!strncmp(line, "tstyle ", 7)) {
            if (trackAt < 0 || clipAt < 0) continue;
            TextStyle &st = p.tracks[trackAt].clips[clipAt].text;
            unsigned fill = 0xffffffffu, outline = 0x000000ffu;
            sscanf(line + 7, "%lf %lf %lf %lf %u %u %lf %d %lf", &st.size, &st.x,
                   &st.y, &st.rotation, &fill, &outline, &st.outlineWidth,
                   &st.align, &st.lineSpacing);
            st.fill = (Rgba)fill;
            st.outline = (Rgba)outline;
        } else if (!strncmp(line, "tfont ", 6)) {
            if (trackAt < 0 || clipAt < 0) continue;
            p.tracks[trackAt].clips[clipAt].text.font = line + 6;
        } else if (!strncmp(line, "tstr ", 5)) {
            if (trackAt < 0 || clipAt < 0) continue;
            p.tracks[trackAt].clips[clipAt].text.text = unescape_text(line + 5);
        }
    }
    fclose(f);

    if (p.tracks.empty()) {
        Project fresh = newProject();
        p.tracks = fresh.tracks;
    }

    /* Ids in the file are trusted, but a file edited by hand might repeat
     * one, and everything downstream looks clips up by id. */
    int maxId = p.nextId - 1;
    for (const BinItem &b : p.bin) maxId = std::max(maxId, b.id);
    for (const Track &t : p.tracks) {
        maxId = std::max(maxId, t.id);
        for (const Clip &c : t.clips) maxId = std::max(maxId, std::max(c.id, c.link));
    }
    p.nextId = maxId + 1;

    for (Track &t : p.tracks)
        std::sort(t.clips.begin(), t.clips.end(),
                  [](const Clip &a, const Clip &b) { return a.pos < b.pos; });

    p.dirty = false;
    *out = p;
    return true;
}

} /* namespace sn */
