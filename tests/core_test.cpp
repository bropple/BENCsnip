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
            remove(s.path.c_str());
        }
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
