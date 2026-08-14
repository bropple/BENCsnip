/*
 * BENCsnip - core tests
 *
 * No window, no sound card. Everything here runs on the half of the program
 * that a headless machine can build, which is the same reason the core is
 * kept free of raylib in the first place.
 *
 * The media tests need files. `make test` makes them with ffmpeg if it is on
 * the PATH and skips those tests if it is not - a machine without the ffmpeg
 * command still builds and still tests the timeline.
 */

#include "sn_export.h"
#include "sn_media.h"
#include "sn_project.h"
#include "sn_render.h"
#include "sn_timeline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define NEAR(a, b, tol) (std::fabs((double)(a) - (double)(b)) <= (tol))

static const char *V1 = "media/test1.mp4";      /* 8 s, 1280x720, 30 fps */
static const char *V2 = "media/test2.webm";     /* 5 s, 640x480, 25 fps  */
static const char *A1 = "media/test3.mp3";      /* 6 s, audio only       */
static const char *G1 = "media/overlay.gif";   /* 0.8 s, transparent    */
static const char *V5 = "media/test5.mp4";      /* 2 s, a gradient       */

static bool have(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ *
 * The timeline, which needs no files at all
 * ------------------------------------------------------------------ */

/* A bin item that refers to nothing, for the edit tests: the model does not
 * open files, so it does not need one to be exercised. */
static int fakeItem(sn::Project &p, double dur, bool video, bool audio)
{
    sn::BinItem b;
    b.id = p.newId();
    b.info.name = "fake";
    b.info.duration = dur;
    b.info.hasVideo = video;
    b.info.hasAudio = audio;
    b.info.width = 1920;
    b.info.height = 1080;
    p.bin.push_back(b);
    return b.id;
}

static void test_timeline()
{
    printf("timeline\n");

    sn::Project p = sn::newProject();
    CHECK(p.tracks.size() == 2, "a new project has a video and an audio track");
    CHECK(p.firstTrack(sn::TRACK_VIDEO) == 0, "video is on top");

    int item = fakeItem(p, 10.0, true, true);
    double end = sn::placeItem(p, item, 0.0);
    CHECK(NEAR(end, 10.0, 1e-9), "placing a 10 s item ends at 10, got %f", end);
    CHECK(p.tracks[0].clips.size() == 1, "one video clip");
    CHECK(p.tracks[1].clips.size() == 1, "one audio clip");
    CHECK(p.tracks[0].clips[0].link != 0, "video and audio are linked");
    CHECK(NEAR(p.duration(), 10.0, 1e-9), "duration is 10");

    /* Split at 4: two clips per track, and the halves add up. */
    int cuts = sn::splitAt(p, 4.0);
    CHECK(cuts == 2, "a split cuts both tracks, got %d", cuts);
    CHECK(p.tracks[0].clips.size() == 2, "video is in two pieces");
    CHECK(NEAR(p.tracks[0].clips[0].dur(), 4.0, 1e-9), "first half is 4 s");
    CHECK(NEAR(p.tracks[0].clips[1].dur(), 6.0, 1e-9), "second half is 6 s");
    CHECK(NEAR(p.tracks[0].clips[1].in, 4.0, 1e-9), "the second half starts 4 s into the source");
    CHECK(NEAR(p.duration(), 10.0, 1e-9), "a split changes nothing about the length");

    /* Delete the first piece with a ripple: everything slides back by 4. */
    sn::ClipRef first{0, p.tracks[0].clips[0].id};
    sn::removeClip(p, first, true);
    CHECK(p.tracks[0].clips.size() == 1, "one video clip left");
    CHECK(NEAR(p.tracks[0].clips[0].pos, 0.0, 1e-9), "and it slid to zero, got %f",
          p.tracks[0].clips[0].pos);
    CHECK(NEAR(p.duration(), 6.0, 1e-9), "duration is 6 now, got %f", p.duration());

    /* Trimming the tail shortens; trimming past the source is refused. */
    sn::ClipRef c0{0, p.tracks[0].clips[0].id};
    sn::trimClip(p, c0, false, 3.0);
    CHECK(NEAR(p.clip(c0)->dur(), 3.0, 1e-9), "trimmed to 3 s");
    sn::trimClip(p, c0, false, 999.0);
    CHECK(p.clip(c0)->out <= 10.0 + 1e-9, "cannot trim past the end of the source, out=%f",
          p.clip(c0)->out);

    /* Dragging the tail past the end of the source loops it rather than
     * refusing to move: the clip gets longer, the range it plays does not,
     * and the source time wraps. */
    {
        sn::Project L = sn::newProject();
        int it = fakeItem(L, 4.0, true, false);
        sn::placeItem(L, it, 0.0);
        sn::ClipRef lr{0, L.tracks[0].clips[0].id};

        sn::trimClip(L, lr, false, 10.0);          /* well past four seconds */
        const sn::Clip *lc = L.clip(lr);

        CHECK(NEAR(lc->dur(), 10.0, 1e-6), "the clip is as long as it was dragged, got %f",
              lc->dur());
        CHECK(NEAR(lc->out, 4.0, 1e-6), "but the range it plays stops at the file's end");
        CHECK(lc->looped(), "and it says it is looping");
        CHECK(NEAR(lc->repeat, 2.5, 1e-6), "two and a half times, got %f", lc->repeat);

        /* Source time wraps: five seconds in is one second into the second
         * pass, not five seconds into a four second file. */
        CHECK(NEAR(lc->srcAt(5.0), 1.0, 1e-6), "the source time wraps, got %f",
              lc->srcAt(5.0));
        CHECK(NEAR(lc->srcAt(9.0), 1.0, 1e-6), "and again on the third pass, got %f",
              lc->srcAt(9.0));
        CHECK(NEAR(lc->srcAt(2.0), 2.0, 1e-6), "the first pass is unchanged");

        /* Dragging it back inside the file stops the looping. */
        sn::trimClip(L, lr, false, 3.0);
        lc = L.clip(lr);
        CHECK(!lc->looped(), "dragging it back in stops the repeat");
        CHECK(NEAR(lc->dur(), 3.0, 1e-6), "and it is just a shorter clip again");
    }

    /* Head trim keeps the content under the mouse still. */
    double srcUnder = p.clip(c0)->srcAt(1.0);
    sn::trimClip(p, c0, true, 1.0);
    CHECK(NEAR(p.clip(c0)->pos, 1.0, 1e-9), "head moved to 1");
    CHECK(NEAR(p.clip(c0)->srcAt(1.0), srcUnder, 1e-9),
          "the same source frame is still at t=1");

    /* Dropping a clip on top of another cuts a hole in it. */
    sn::Project q = sn::newProject();
    int it2 = fakeItem(q, 10.0, true, false);
    sn::placeItem(q, it2, 0.0);
    sn::Clip over;
    over.source = it2;
    over.in = 0;
    over.out = 2;
    over.pos = 4;
    sn::addClip(q, 0, over);
    CHECK(q.tracks[0].clips.size() == 3, "the covered clip became two, got %d",
          (int)q.tracks[0].clips.size());
    CHECK(NEAR(q.tracks[0].clips[0].end(), 4.0, 1e-9), "the head ends where the drop starts");
    CHECK(NEAR(q.tracks[0].clips[2].pos, 6.0, 1e-9), "the tail starts where the drop ends");
    CHECK(NEAR(q.duration(), 10.0, 1e-9), "and the whole thing is still 10 s");

    /* Undo. */
    sn::History h;
    h.reset(q);
    double before = q.duration();
    sn::removeClip(q, sn::ClipRef{0, q.tracks[0].clips[0].id}, false);
    h.commit(q);
    CHECK(q.tracks[0].clips.size() == 2, "deleted one");
    CHECK(h.undo(&q), "undo works");
    CHECK(q.tracks[0].clips.size() == 3, "and it came back");
    CHECK(NEAR(q.duration(), before, 1e-9), "with the same duration");
    CHECK(h.redo(&q), "redo works");
    CHECK(q.tracks[0].clips.size() == 2, "and took it away again");

    /* Snapping. */
    double s = sn::snap(q, 3.98, 0.1, -1);
    CHECK(NEAR(s, 4.0, 1e-9), "3.98 snaps to the cut at 4, got %f", s);
    s = sn::snap(q, 3.5, 0.1, -1);
    CHECK(NEAR(s, 3.5, 1e-9), "3.5 snaps to nothing, got %f", s);
}

/* ------------------------------------------------------------------ *
 * Media
 * ------------------------------------------------------------------ */

static void test_media()
{
    printf("media\n");

    sn::MediaInfo mi;
    std::string err;
    CHECK(sn::probe(V1, &mi, &err), "probe %s: %s", V1, err.c_str());
    CHECK(mi.hasVideo && mi.hasAudio, "test1 has both");
    CHECK(mi.width == 1280 && mi.height == 720, "1280x720, got %dx%d", mi.width, mi.height);
    CHECK(NEAR(mi.fps, 30.0, 0.01), "30 fps, got %f", mi.fps);
    CHECK(NEAR(mi.duration, 8.0, 0.1), "8 s, got %f", mi.duration);

    CHECK(!sn::probe("tests/core_test.cpp", &mi, &err), "a .cpp is not media");

    sn::Source *s = sn::Source::open(V1, &err);
    CHECK(s != nullptr, "open %s: %s", V1, err.c_str());
    if (!s) return;

    /* Seeking backwards and forwards lands on the frame asked for. */
    const double at[] = {0.0, 5.0, 1.0, 7.5, 2.5};
    for (double t : at) {
        sn::VideoFrame f;
        CHECK(s->frameAt(t, &f, 320, 180), "a frame at %.1f", t);
        CHECK(f.w == 320 && f.h == 180, "scaled to 320x180, got %dx%d", f.w, f.h);
        CHECK(NEAR(f.pts, t, 1.0 / 30.0 + 1e-3), "frame at %.2f has pts %.3f", t, f.pts);
    }

    /* Audio comes out continuously: two adjacent blocks should not repeat or
     * skip, which shows up as a discontinuity at the join. */
    const int N = 1024;
    std::vector<float> b1(N * sn::CHANS), b2(N * sn::CHANS);
    s->audioAt(2.0, N, b1.data());
    s->audioAt(2.0 + N / (double)sn::RATE, N, b2.data());

    double e1 = 0, e2 = 0;
    for (int i = 0; i < N * sn::CHANS; i++) { e1 += b1[i] * b1[i]; e2 += b2[i] * b2[i]; }
    CHECK(e1 > 1e-6, "the first block has sound in it");
    CHECK(e2 > 1e-6, "so does the second");

    /* A 440 Hz sine at 48 kHz moves by at most this much per sample, so a
     * jump much larger than that at the seam is a dropped or repeated
     * block. */
    double seam = std::fabs(b2[0] - b1[(N - 1) * sn::CHANS]);
    CHECK(seam < 0.2, "the blocks join without a step, got %f", seam);

    delete s;

    /* An audio-only file has no video and says so rather than failing. */
    sn::Source *a = sn::Source::open(A1, &err);
    CHECK(a != nullptr, "open %s", A1);
    if (a) {
        CHECK(!a->hasVideo(), "an mp3 has no video");
        CHECK(a->hasAudio(), "an mp3 has audio");
        sn::VideoFrame f;
        CHECK(!a->frameAt(1.0, &f, 64, 64), "and asking for a frame is a no, not a crash");
        delete a;
    }
}

/* ------------------------------------------------------------------ *
 * Render
 * ------------------------------------------------------------------ */

static void test_render()
{
    printf("render\n");

    sn::Project p = sn::newProject();
    std::string err;
    int a = sn::importFile(p, V1, &err);
    CHECK(a != 0, "import %s: %s", V1, err.c_str());
    int b = sn::importFile(p, V2, &err);
    CHECK(b != 0, "import %s: %s", V2, err.c_str());
    if (!a || !b) return;

    CHECK(p.width == 1280 && p.height == 720, "the project took its size from the first video");

    sn::placeItem(p, a, 0.0);
    sn::placeItem(p, b, 8.0);
    CHECK(NEAR(p.duration(), 13.0, 0.2), "8 + 5 = 13, got %f", p.duration());

    sn::Renderer r(&p);
    sn::VideoFrame f;

    CHECK(r.videoAt(2.0, 320, 180, &f), "something is under 2 s");
    CHECK(f.w == 320 && f.h == 180, "the frame is the size asked for");

    /* test2 is 4:3 in a 16:9 project, so it must be pillarboxed - the left
     * column black and the middle not. */
    CHECK(r.videoAt(10.0, 320, 180, &f), "something is under 10 s");
    const uint8_t *px = f.rgba.data();
    auto at = [&](int x, int y) { return px + ((size_t)y * f.w + x) * 4; };
    CHECK(at(2, 90)[0] < 8 && at(2, 90)[1] < 8, "the left edge is black (pillarbox)");
    bool colour = false;
    for (int x = 40; x < 280; x += 8)
        if (at(x, 90)[0] > 16 || at(x, 90)[1] > 16 || at(x, 90)[2] > 16) colour = true;
    CHECK(colour, "the middle is not");

    /* Two tracks side by side: each at half scale, one hard left, one hard
     * right. What has to be true is that the left half of the canvas is the
     * left track's picture and the right half is the right track's, with the
     * bands above and below them black - which is what a viewer would call
     * side by side, and what nothing else in the program checks. */
    {
        sn::Project sbs = sn::newProject();
        std::string e2;
        int ia = sn::importFile(sbs, V1, &e2);
        int ib = sn::importFile(sbs, V2, &e2);
        sbs.width = 640;
        sbs.height = 360;

        sn::addTrack(sbs, sn::TRACK_VIDEO, 0);
        sn::placeItem(sbs, ia, 0.0, 0, -2);          /* video only, back track */
        sn::placeItem(sbs, ib, 0.0, 1, -2);          /* video only, front      */

        sbs.tracks[0].scaleX = sbs.tracks[0].scaleY = 0.5;
        sbs.tracks[0].x = -1.0;                      /* hard left  */
        sbs.tracks[1].scaleX = sbs.tracks[1].scaleY = 0.5;
        sbs.tracks[1].x = 1.0;                       /* hard right */

        sn::Renderer r2(&sbs);
        sn::VideoFrame f2;
        CHECK(r2.videoAt(1.0, 640, 360, &f2), "both halves have something under them");

        auto px = [&](int x, int y) { return f2.rgba.data() + ((size_t)y * f2.w + x) * 4; };
        auto lit = [&](int x, int y) {
            const uint8_t *p = px(x, y);
            return p[0] > 12 || p[1] > 12 || p[2] > 12;
        };

        CHECK(lit(160, 180), "the left half is drawn");
        CHECK(lit(480, 180), "the right half is drawn");
        CHECK(!lit(320, 10), "and the top of the canvas is still black");
        CHECK(!lit(320, 350), "as is the bottom");
    }

    /* Transparency. A GIF with an alpha channel laid over a video has to show
     * the video through it - the decoder puts a colour in those pixels and
     * marks them alpha 0, and a blit that copies three channels of four
     * paints that colour anyway. Which is how a rotating logo arrives as a
     * white box. */
    if (have(G1)) {
        sn::Project al = sn::newProject();
        std::string e4;
        int bg = sn::importFile(al, V1, &e4);
        int ov = sn::importFile(al, G1, &e4);
        al.width = 320;
        al.height = 180;

        sn::placeItem(al, bg, 0.0, 0, -2);              /* the back  */
        const int front = sn::addTrack(al, sn::TRACK_VIDEO, 1);
        sn::placeItem(al, ov, 0.0, front, -2);          /* the front */
        al.tracks[front].scaleX = al.tracks[front].scaleY = 0.5;

        /* Rendered twice: once with the overlay hidden, once with it shown.
         *
         * The test is then "a transparent pixel is identical to what was
         * behind it", which needs to know no colours at all. The first
         * version of this checked that the pixel was "not black" - and the
         * transparent entry in a GIF's palette is usually white, so it passed
         * whether the alpha was honoured or not. It tested nothing, and said
         * it tested transparency. */
        sn::Renderer r4(&al);
        sn::VideoFrame behind, both;

        al.tracks[front].muted = true;
        CHECK(r4.videoAt(0.2, 320, 180, &behind), "the background draws on its own");
        al.tracks[front].muted = false;
        CHECK(r4.videoAt(0.2, 320, 180, &both), "the overlay draws");

        auto at4 = [&](const sn::VideoFrame &f, int x, int y) {
            return f.rgba.data() + ((size_t)y * f.w + x) * 4;
        };

        /* Where those two points are, worked out rather than guessed at.
         *
         * The overlay is a 240x240 square source at half scale on a 320x180
         * canvas: fitted into 160x90 it becomes 90x90, centred, so it covers
         * x 115..205 and y 45..135. Its gold square is the middle half of the
         * source, so it lands at x 137..182, y 67..112.
         *
         * The first sample is therefore inside the layer and outside the
         * gold - transparent, and the whole point of the test. The earlier
         * version of this sampled a point outside the layer altogether, which
         * showed the video whether alpha worked or not, and passed for a
         * reason that had nothing to do with what it claimed to check. */
        const uint8_t *clearBg = at4(behind, 120, 50);
        const uint8_t *clearUp = at4(both, 120, 50);
        const uint8_t *solidBg = at4(behind, 160, 90);
        const uint8_t *solidUp = at4(both, 160, 90);

        CHECK(clearUp[0] == clearBg[0] && clearUp[1] == clearBg[1] &&
                  clearUp[2] == clearBg[2],
              "a transparent pixel is exactly what was behind it: got %d,%d,%d, "
              "behind is %d,%d,%d",
              clearUp[0], clearUp[1], clearUp[2], clearBg[0], clearBg[1], clearBg[2]);

        CHECK(solidUp[0] != solidBg[0] || solidUp[1] != solidBg[1] ||
                  solidUp[2] != solidBg[2],
              "and an opaque one is not");
        CHECK(solidUp[0] > 180 && solidUp[1] > 150 && solidUp[2] < 110,
              "it is the overlay's own colour (%d,%d,%d)", solidUp[0], solidUp[1],
              solidUp[2]);
    }

    /* Cropping changes the shape that gets fitted, so a source cropped to a
     * narrow band comes out as a narrow band rather than a squashed frame. */
    {
        sn::Project cr = sn::newProject();
        std::string e3;
        int ia = sn::importFile(cr, V1, &e3);
        cr.width = 640;
        cr.height = 360;
        sn::placeItem(cr, ia, 0.0, 0, -2);

        cr.tracks[0].cropT = 0.4;
        cr.tracks[0].cropB = 0.4;      /* keep the middle fifth */

        sn::Renderer r3(&cr);
        sn::VideoFrame f3;
        CHECK(r3.videoAt(1.0, 640, 360, &f3), "the cropped track still draws");

        auto lit3 = [&](int x, int y) {
            const uint8_t *p = f3.rgba.data() + ((size_t)y * f3.w + x) * 4;
            return p[0] > 12 || p[1] > 12 || p[2] > 12;
        };
        CHECK(lit3(320, 180), "the middle is the kept band");
        CHECK(!lit3(320, 20), "and what was cropped away is not drawn");
    }

    /* Past the end is black, and does not fail. */
    r.videoAt(100.0, 64, 36, &f);
    CHECK(f.valid(), "past the end still returns a frame");
    CHECK(f.rgba[0] == 0 && f.rgba[1] == 0 && f.rgba[2] == 0, "and it is black");

    /* The mix has sound where a clip is and silence where none is. */
    std::vector<float> mix(1024 * sn::CHANS);
    r.audioAt(2.0, 1024, mix.data());
    double e = 0;
    for (float v : mix) e += v * v;
    CHECK(e > 1e-6, "there is audio at 2 s");

    r.audioAt(100.0, 1024, mix.data());
    e = 0;
    for (float v : mix) e += v * v;
    CHECK(e == 0.0, "and silence past the end");

    /* A fade halves the level at its midpoint. */
    p.tracks[1].clips[0].fadeIn = 2.0;
    r.audioAt(1.0, 1024, mix.data());
    double faded = 0;
    for (float v : mix) faded += v * v;
    r.audioAt(1.0, 1024, mix.data());   /* same place, fade removed below */
    p.tracks[1].clips[0].fadeIn = 0.0;
    r.audioAt(1.0, 1024, mix.data());
    double full = 0;
    for (float v : mix) full += v * v;
    CHECK(faded < full * 0.5, "a fade at its midpoint is quieter, %f vs %f", faded, full);
}

/* ------------------------------------------------------------------ *
 * Project file
 * ------------------------------------------------------------------ */

static void test_project()
{
    printf("project file\n");

    sn::Project p = sn::newProject();
    std::string err;
    int a = sn::importFile(p, V1, &err);
    if (!a) { CHECK(false, "import: %s", err.c_str()); return; }

    sn::placeItem(p, a, 1.0);
    sn::splitAt(p, 3.0);
    p.tracks[1].clips[0].gain = 0.5;
    p.tracks[0].clips[0].fadeOut = 0.4;
    p.name = "a test";

    const std::string path = "media/test.bencsnip";
    CHECK(sn::saveProject(p, path, &err), "save: %s", err.c_str());

    sn::Project q;
    CHECK(sn::loadProject(&q, path, &err), "load: %s", err.c_str());
    CHECK(q.name == "a test", "the name came back, got '%s'", q.name.c_str());
    CHECK(q.bin.size() == p.bin.size(), "the bin came back");
    CHECK(q.tracks.size() == p.tracks.size(), "the tracks came back");
    CHECK(NEAR(q.duration(), p.duration(), 1e-6), "the duration is the same");
    CHECK(q.tracks[0].clips.size() == p.tracks[0].clips.size(), "the cuts came back");
    CHECK(NEAR(q.tracks[1].clips[0].gain, 0.5, 1e-4), "so did the gain");
    CHECK(NEAR(q.tracks[0].clips[0].fadeOut, 0.4, 1e-4), "and the fade");
    CHECK(!q.dirty, "a project just loaded is not dirty");

    remove(path.c_str());
}

/* ------------------------------------------------------------------ *
 * Export
 * ------------------------------------------------------------------ */

static void test_export()
{
    printf("export\n");

    if (!sn::haveEncoder("libx264")) {
        printf("  (no libx264 in this ffmpeg - skipping the render test)\n");
    } else {
        sn::Project p = sn::newProject();
        std::string err;
        int a = sn::importFile(p, V1, &err);
        int b = sn::importFile(p, V2, &err);
        if (!a || !b) { CHECK(false, "import: %s", err.c_str()); return; }

        /* Two sources, a cut and a fade: everything that forces a render. */
        sn::placeItem(p, a, 0.0);
        sn::placeItem(p, b, 3.0);
        p.tracks[0].clips[0].fadeOut = 0.5;

        sn::ExportSettings s;
        s.path = "media/out_render.mp4";
        s.width = 640;
        s.height = 360;
        s.fps = 30;
        s.crf = 30;
        s.preset = "ultrafast";

        std::string why;
        CHECK(!sn::canStreamCopy(p, s, &why), "two sources cannot be copied");

        sn::ExportStatus st;
        bool ok = sn::exportTimeline(p, s, &st);
        CHECK(ok, "render: %s", st.said().c_str());
        CHECK(!st.copied.load(), "and it rendered rather than copied");

        if (ok) {
            sn::MediaInfo mi;
            CHECK(sn::probe(s.path, &mi, &err), "the output is readable: %s", err.c_str());
            CHECK(mi.hasVideo && mi.hasAudio, "with both streams");
            CHECK(mi.width == 640 && mi.height == 360, "at 640x360, got %dx%d",
                  mi.width, mi.height);
            CHECK(NEAR(mi.duration, p.duration(), 0.35),
                  "and the length of the timeline (%.2f), got %.2f", p.duration(), mi.duration);

            /* The estimate against what actually came out.
             *
             * The bounds are wide on purpose. At constant quality the encoder
             * decides the bitrate from the picture, and these test files are
             * flat colour bars - about as compressible as video gets, and
             * measured at 1.8x under the estimate. Real footage lands much
             * closer. What this catches is not inaccuracy but nonsense: a
             * change that makes it off by a hundred, or zero. */
            bool exact = false;
            const int64_t guess = sn::estimateSize(p, s, &exact);
            CHECK(!exact, "a render is an estimate, not a promise");
            CHECK(guess > mi.bytes / 4 && guess < mi.bytes * 6,
                  "the estimate is in the right country: %lld vs %lld actual",
                  (long long)guess, (long long)mi.bytes);

            remove(s.path.c_str());
        }
    }

    /* Audio only, into a container that will not take a video stream at all.
     * The export has to notice and write one stream rather than failing at
     * the muxer. */
    {
        sn::Project p = sn::newProject();
        std::string err;
        int a = sn::importFile(p, V1, &err);
        int b = sn::importFile(p, A1, &err);
        if (!a || !b) { CHECK(false, "import: %s", err.c_str()); return; }

        sn::placeItem(p, a, 0.0);
        sn::placeItem(p, b, 1.0);

        sn::ExportSettings s;
        s.path = "media/out_audio.wav";
        s.vcodec = "";
        s.acodec = "pcm_s16le";

        sn::ExportStatus st;
        bool ok = sn::exportTimeline(p, s, &st);
        CHECK(ok, "audio export: %s", st.said().c_str());

        if (ok) {
            sn::MediaInfo mi;
            CHECK(sn::probe(s.path, &mi, &err), "the wav is readable: %s", err.c_str());
            CHECK(mi.hasAudio && !mi.hasVideo, "audio only");
            CHECK(mi.rate == sn::RATE && mi.chans == sn::CHANS,
                  "48 kHz stereo, got %d Hz %d ch", mi.rate, mi.chans);
            CHECK(NEAR(mi.duration, p.duration(), 0.2), "as long as the timeline (%.2f), got %.2f",
                  p.duration(), mi.duration);
            remove(s.path.c_str());
        }
    }

    /* Exporting a range rather than the whole timeline. */
    {
        sn::Project p = sn::newProject();
        std::string err;
        int a = sn::importFile(p, V1, &err);
        if (!a) { CHECK(false, "import: %s", err.c_str()); return; }
        sn::placeItem(p, a, 0.0);

        sn::ExportSettings s;
        s.path = "media/out_range.mp4";
        s.width = 320;
        s.height = 180;
        s.fps = 30;
        s.crf = 34;
        s.preset = "ultrafast";
        s.from = 2.0;
        s.to = 5.0;
        s.allowCopy = false;

        sn::ExportStatus st;
        bool ok = sn::exportTimeline(p, s, &st);
        CHECK(ok, "range export: %s", st.said().c_str());

        if (ok) {
            sn::MediaInfo mi;
            CHECK(sn::probe(s.path, &mi, &err), "readable");
            CHECK(NEAR(mi.duration, 3.0, 0.25), "three seconds of it, got %.2f", mi.duration);
            remove(s.path.c_str());
        }
    }

    /* The size estimate. Exact for a copy - it is the source's own bitrate
     * over the range - and in the right order of magnitude for a render,
     * which is all a number labelled "about" has to be. */
    {
        sn::Project p = sn::newProject();
        std::string err;
        int a = sn::importFile(p, V1, &err);
        if (!a) { CHECK(false, "import: %s", err.c_str()); return; }
        sn::placeItem(p, a, 0.0);

        sn::MediaInfo mi;
        sn::probe(V1, &mi, &err);

        sn::ExportSettings s;
        s.path = "media/out_est.mp4";
        s.width = p.width;
        s.height = p.height;
        s.fps = p.fps;

        bool exact = false;
        int64_t whole = sn::estimateSize(p, s, &exact);
        CHECK(exact, "a straight copy of the whole thing is an exact size");
        CHECK(NEAR(whole, mi.bytes, mi.bytes * 0.02),
              "and it is the file's own size (%lld vs %lld)", (long long)whole,
              (long long)mi.bytes);

        /* Half the range is half the bytes. */
        s.to = mi.duration * 0.5;
        int64_t half = sn::estimateSize(p, s, &exact);
        CHECK(NEAR(half, whole / 2, whole * 0.05), "half the range is half the size");

        /* Re-encoding cannot be exact, and must still be sane: bigger at
         * higher quality, smaller at lower. */
        s.to = -1;
        s.allowCopy = false;
        s.crf = 18;
        int64_t good = sn::estimateSize(p, s, &exact);
        CHECK(!exact, "a render is a guess and says so");
        s.crf = 32;
        int64_t rough = sn::estimateSize(p, s, &exact);
        CHECK(good > rough, "better quality estimates bigger (%lld vs %lld)",
              (long long)good, (long long)rough);
        CHECK(rough > 0, "and neither is zero");
    }

    /* The fast path: one clip, trimmed, same size, same codecs. */
    {
        sn::Project p = sn::newProject();
        std::string err;
        int a = sn::importFile(p, V1, &err);
        if (!a) { CHECK(false, "import: %s", err.c_str()); return; }
        sn::placeItem(p, a, 0.0);

        sn::ClipRef v{0, p.tracks[0].clips[0].id};
        sn::trimClip(p, v, false, 4.0);      /* keep the first four seconds */

        sn::ExportSettings s;
        s.path = "media/out_copy.mp4";
        s.width = p.width;
        s.height = p.height;
        s.fps = p.fps;

        std::string why;
        CHECK(sn::canStreamCopy(p, s, &why), "one trimmed clip can be copied: %s", why.c_str());

        sn::ExportStatus st;
        bool ok = sn::exportTimeline(p, s, &st);
        CHECK(ok, "copy: %s", st.said().c_str());
        CHECK(st.copied.load(), "and it took the fast path");

        if (ok) {
            sn::MediaInfo mi;
            CHECK(sn::probe(s.path, &mi, &err), "the output is readable");
            CHECK(mi.vcodec == "h264", "the video was not re-encoded, got %s", mi.vcodec.c_str());
            CHECK(NEAR(mi.duration, 4.0, 0.5), "about four seconds, got %.2f", mi.duration);
            remove(s.path.c_str());
        }
    }

    /* --- a GIF, which is the one output that has to choose its own colours ---
     *
     * Written and then read back and compared against the frames it was made
     * of, because the failure this is here for does not look like a failure
     * anywhere else: the file is valid, the right length, the right size, and
     * the picture in it is wrong. Handing the encoder the pixel format it
     * lists first gets a fixed 3-3-2 palette and a mean channel error near 17
     * on a gradient, which is a visibly banded GIF and a passing test in every
     * other sense. */
    if (have(V5)) {
        sn::Project p = sn::newProject();
        std::string err;
        int id = sn::importFile(p, V5, &err);
        if (!id) { CHECK(false, "import: %s", err.c_str()); return; }
        sn::placeItem(p, id, 0.0);

        const int W = 320, H = 240;
        p.width = W; p.height = H; p.fps = 10;

        sn::ExportSettings s;
        s.path = "media/out_gif.gif";
        s.width = W; s.height = H; s.fps = 10;
        s.vcodec = "gif";
        s.acodec = "";
        s.allowCopy = false;
        s.from = 0; s.to = 2.0;

        sn::ExportStatus st;
        const bool ok = sn::exportTimeline(p, s, &st);
        CHECK(ok, "gif: %s", st.said().c_str());

        if (ok) {
            sn::MediaInfo mi;
            CHECK(sn::probe(s.path, &mi, &err), "the gif is readable: %s", err.c_str());
            CHECK(mi.width == W && mi.height == H, "at %dx%d, got %dx%d", W, H,
                  mi.width, mi.height);

            sn::Source *src = sn::Source::open(s.path, &err);
            CHECK(src != nullptr, "and decodes: %s", err.c_str());

            if (src) {
                sn::Renderer ren(&p);
                sn::VideoFrame want, got;
                double sum = 0;
                long n = 0;

                for (int i = 0; i < 10; i++) {
                    const double t = (i + 0.5) / 10.0;
                    if (!ren.videoAt(t, W, H, &want)) continue;
                    if (!src->frameAt(t, &got, W, H)) continue;
                    for (long k = 0; k < (long)W * H; k++) {
                        const uint8_t *a = want.rgba.data() + k * 4;
                        const uint8_t *b = got.rgba.data() + k * 4;
                        for (int c = 0; c < 3; c++)
                            sum += std::fabs((double)a[c] - (double)b[c]);
                        n += 3;
                    }
                }
                const double e = n ? sum / n : 999.0;

                /* Measured at 1.5 with a palette chosen from the footage and
                 * 16.9 without. Anything under 6 is a palette that was looked
                 * at; the gap is wide enough that this does not fail over a
                 * rounding change in the dither. */
                CHECK(e < 6.0, "the gif is close to the frames it was made of "
                               "(mean channel error %.2f, 3-3-2 scores 16.9)", e);
                delete src;
            }
            remove(s.path.c_str());
        }
    }
}

int main()
{
    printf("BENCsnip core tests\n\n");

    test_timeline();

    if (have(V1) && have(V2) && have(A1)) {
        test_media();
        test_render();
        test_project();
        test_export();
    } else {
        printf("\n  (no test media in media/ - skipping everything that decodes.\n");
        printf("   `make testmedia` writes them with ffmpeg.)\n");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
