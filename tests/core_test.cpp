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
#include "sn_text.h"
#include "sn_timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
static const char *V6 = "media/test6.mp4";      /* 2 s, silent audio     */
static const char *G2 = "media/test7.gif";      /* 0.8 s, 8 moving frames */

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

/* ------------------------------------------------------------------ *
 * Looping
 *
 * Needs no media: it is arithmetic on a Clip, and the arithmetic is what was
 * wrong. See Clip::srcAt.
 * ------------------------------------------------------------------ */

static void test_loop()
{
    printf("looping\n");

    /* A 6.4 second cycle sampled the way the exporter samples it - from + n
     * over fps, which is a division and does not land on the same doubles
     * that 6.4 times n does. Every wrap has to come back to the start of the
     * cycle, not to a hair short of the end of it, because a hair short of
     * the end is past the last frame and the decoder has nothing there. */
    sn::Clip c;
    c.in = 0.0;
    c.out = 6.4;
    c.pos = 0.0;
    c.repeat = 8.0;

    const double fps = 60.0;
    int late = 0, worst = -1;
    double worstVal = 0;
    for (int i = 0; i < (int)(fps * c.dur()); i++) {
        const double t = i / fps;
        const double s = c.srcAt(t);
        if (s < c.in - 1e-9 || s > c.out + 1e-9) { late++; continue; }
        /* The only way to be within a microsecond of the end is to have
         * failed to wrap: a sample lands every 1/60 of a second. */
        if (s > c.out - 1e-6) {
            late++;
            if (worst < 0) { worst = i; worstVal = s; }
        }
    }
    CHECK(late == 0, "no sample lands at the far end of the cycle, %d did "
                     "(first at frame %d, src %.9f)", late, worst, worstVal);

    /* The wrap still wraps: three and a half cycles in is half a cycle in. */
    CHECK(NEAR(c.srcAt(6.4 * 3 + 3.2), 3.2, 1e-6), "and it still wraps, got %f",
          c.srcAt(6.4 * 3 + 3.2));
    CHECK(NEAR(c.srcAt(0.0), 0.0, 1e-9), "the start is the start");
    CHECK(NEAR(c.srcAt(1.0), 1.0, 1e-9), "the first pass is not wrapped");

    /* A clip that does not repeat is not wrapped at all, however far past the
     * cycle it is asked. */
    sn::Clip once = c;
    once.repeat = 1.0;
    CHECK(NEAR(once.srcAt(6.0), 6.0, 1e-9), "a clip that plays once does not wrap");
}

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

    /* A video whose audio track is nothing but silence has no audio, and must
     * not put a clip on the timeline for it. Whether the sound is real is not
     * something the container says - the stream is there either way - so this
     * is the one property here that costs a decode to answer. */
    if (have(V6)) {
        sn::MediaInfo si;
        CHECK(sn::probe(V6, &si, &err), "probe %s: %s", V6, err.c_str());
        CHECK(si.hasVideo, "the silent one still has its picture");
        CHECK(!si.hasAudio, "and no audio, because there is none in it");
        CHECK(si.silentAudio, "and it says that is why");

        sn::Project sp = sn::newProject();
        const int sid = sn::importFile(sp, V6, &err);
        CHECK(sid != 0, "it still imports: %s", err.c_str());
        sn::placeItem(sp, sid, 0.0);

        int ac = 0;
        for (const sn::Track &t : sp.tracks)
            if (t.kind == sn::TRACK_AUDIO) ac += (int)t.clips.size();
        CHECK(ac == 0, "and lays down no audio clip, got %d", ac);
    }

    /* Playing a GIF to the end and going back to the start - which is what a
     * looped clip does at every wrap - has to give back the frames it gave
     * the first time.
     *
     * Worth its own test because that rewind does not go through libav's
     * seek. A GIF has no index, so the generic seek walks the file and costs
     * more the bigger it is: sixteen milliseconds on a nine megabyte one,
     * every pass, landing as a hitch exactly when the animation comes round.
     * Rewinding the byte stream instead is free, and this is the check that
     * it is also right - the same shortcut on an mp4 hands back a picture
     * from the middle of the header. */
    if (have(G2)) {
        std::string ge;
        std::vector<std::vector<uint8_t>> ref;

        sn::Source *a = sn::Source::open(G2, &ge);
        CHECK(a != nullptr, "open %s: %s", G2, ge.c_str());
        if (a) {
            sn::VideoFrame f;
            for (int i = 0; i < 8; i++) {
                if (!a->frameAt(i * 0.1, &f, 80, 60)) break;
                ref.push_back(f.rgba);
            }
            delete a;
        }
        CHECK(ref.size() == 8, "eight frames on the first pass, got %d", (int)ref.size());

        /* Not all the same picture, or everything below passes for free. */
        int distinct = 0;
        for (size_t i = 1; i < ref.size(); i++)
            if (ref[i] != ref[i - 1]) distinct++;
        CHECK(distinct >= 6, "and they are different pictures, got %d changes", distinct);

        sn::Source *b = sn::Source::open(G2, &ge);
        if (b && ref.size() == 8) {
            sn::VideoFrame f;
            for (int i = 0; i < 8; i++) b->frameAt(i * 0.1, &f, 80, 60);   /* to the end */

            int wrong = 0;
            for (int i = 0; i < 4; i++) {                                  /* and round */
                if (!b->frameAt(i * 0.1, &f, 80, 60) || f.rgba != ref[i]) wrong++;
            }
            CHECK(wrong == 0, "the wrap gives back the first pass, %d frames differ", wrong);
        }
        delete b;
    }

    /* The other half of that: real sound is not thrown away by the check. */
    {
        sn::MediaInfo li;
        CHECK(sn::probe(V1, &li, &err) && li.hasAudio && !li.silentAudio,
              "a file with sound in it keeps its audio");
    }

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

    /* A timeline running faster than its footage.
     *
     * Sixty frames asked for across one second of a thirty frame clip: each
     * source frame is wanted twice, and the second time it is already sitting
     * scaled in the buffer being handed over, so nothing is decoded or
     * scaled. That shortcut is worth about half the work of playing a clip at
     * double its own rate, and it has exactly one way to go wrong, which is
     * to keep handing back a picture after the moment for it has passed.
     *
     * So this counts how many different pictures come out. Thirty is right.
     * One means the preview has frozen; sixty means every frame was scaled
     * twice and the shortcut is not working. */
    {
        sn::Project fast = sn::newProject();
        std::string e3;
        int id = sn::importFile(fast, V1, &e3);
        sn::placeItem(fast, id, 0.0, 0, -2);
        fast.width = 320;
        fast.height = 180;

        sn::Renderer r3(&fast);
        sn::VideoFrame f3;

        std::vector<uint64_t> seen;
        for (int i = 0; i < 60; i++) {
            if (!r3.videoAt(1.0 + i / 60.0, 320, 180, &f3)) continue;
            /* Cheap and good enough to tell two frames of colour bars apart:
             * every byte added up, which changes whenever anything moves. */
            uint64_t sum = 0;
            for (size_t k = 0; k < f3.rgba.size(); k += 4)
                sum += f3.rgba[k] * 3u + f3.rgba[k + 1] * 5u + f3.rgba[k + 2] * 7u;
            seen.push_back(sum);
        }
        CHECK(seen.size() == 60, "sixty frames came out, got %d", (int)seen.size());

        std::vector<uint64_t> uniq = seen;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());

        CHECK(uniq.size() >= 25 && uniq.size() <= 35,
              "a 30 fps clip at 60 fps gives about 30 different pictures, got %d",
              (int)uniq.size());
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

    /* --- a caption on the picture ---
     *
     * A text track is composited with the video ones, in list order, which is
     * what makes a caption something that can be put behind one layer and in
     * front of another. Here it goes in front, so it must change the frame.
     *
     * Every check is the same frame with the text track muted against it
     * unmuted, rather than against a colour that ought to be unique. It is
     * not: the first version of this looked for magenta pixels, and testsrc2
     * is colour bars, one of which is magenta. Muting is also exactly the
     * question being asked - does the caption draw - so it tests muting for
     * free rather than needing its own case.
     */
    {
        const int ti = sn::addTrack(p, sn::TRACK_TEXT);
        CHECK(ti >= 0, "a text track can be added");
        CHECK(p.tracks[ti].kind == sn::TRACK_TEXT, "and is one");
        CHECK(sn::visualTrack(p.tracks[ti].kind), "and is in the visual band");

        sn::Clip cap;
        cap.id = p.newId();
        cap.in = 0.0;
        cap.out = 4.0;
        cap.pos = 0.0;
        cap.text.text = "HELLO";
        cap.text.size = 0.5;
        cap.text.fill = 0xff00ffffu;
        cap.text.outlineWidth = 0.0;
        CHECK(sn::addClip(p, ti, cap) != nullptr, "and holds a caption");

        auto frame = [&](double t, bool on) {
            p.tracks[ti].muted = !on;
            sn::VideoFrame f;
            r.videoAt(t, 160, 90, &f);
            return f.rgba;
        };
        auto differing = [](const std::vector<uint8_t> &x,
                            const std::vector<uint8_t> &y) {
            int n = 0;
            for (size_t i = 0; i + 3 < x.size() && i + 3 < y.size(); i += 4)
                if (x[i] != y[i] || x[i + 1] != y[i + 1] || x[i + 2] != y[i + 2]) n++;
            return n;
        };

        const std::vector<uint8_t> off1 = frame(1.0, false);
        const std::vector<uint8_t> on1 = frame(1.0, true);
        const int drew = differing(off1, on1);
        CHECK(drew > 0, "the caption changed the frame, %d pixels", drew);

        /* And in its own colour - counted only among the pixels it changed,
         * so the colour bars underneath cannot answer for it. */
        int mine = 0;
        for (size_t i = 0; i + 3 < on1.size(); i += 4) {
            if (off1[i] == on1[i] && off1[i + 1] == on1[i + 1] &&
                off1[i + 2] == on1[i + 2])
                continue;
            if (on1[i] > 200 && on1[i + 1] < 60 && on1[i + 2] > 200) mine++;
        }
        CHECK(mine > 0, "in the colour it was given, %d pixels", mine);

        /* Past the end of the caption there is no caption, so muting it
         * changes nothing at all. */
        const std::vector<uint8_t> off6 = frame(6.0, false);
        const std::vector<uint8_t> on6 = frame(6.0, true);
        CHECK(differing(off6, on6) == 0, "and nothing is drawn after it ends");

        /* The last text track comes out again, unlike the last video or audio
         * one: a project with no captions in it is an ordinary project. */
        CHECK(sn::removeTrack(p, ti), "and the last text track can be removed");
        for (const sn::Track &x : p.tracks)
            CHECK(x.kind != sn::TRACK_TEXT, "leaving none behind");
    }

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

    /* The track's level multiplies the clip's. Half the level is a quarter of
     * the energy, which is the arithmetic and is also the check: a fader
     * wired to the wrong side of the sum, or applied twice, misses it.
     *
     * Two blocks per reading, not one. Asking the mixer for the same second
     * twice does not hand back the same block: the source keeps a fifo of
     * whatever the last decoded frame had left over, so consecutive reads at
     * one time alternate between two phases of it - about 5.2 and 7.8 here.
     * One block against one block is a coin toss over whether the two
     * readings came from the same phase, and the pair covers both either way.
     * The fade check above sidesteps the same thing by reading an extra time
     * and throwing it away. */
    auto energy2 = [&]() {
        double sum = 0;
        for (int k = 0; k < 2; k++) {
            r.audioAt(1.0, 1024, mix.data());
            for (float v : mix) sum += v * v;
        }
        return sum;
    };

    p.tracks[1].gain = 1.0;
    const double unity = energy2();
    p.tracks[1].gain = 0.5;
    const double halved = energy2();
    CHECK(NEAR(halved, unity * 0.25, unity * 0.02),
          "a track at half level is a quarter of the energy, %f vs %f", halved,
          unity * 0.25);

    p.tracks[1].gain = 0.0;
    e = energy2();
    CHECK(e == 0.0, "and all the way down is silence");

    p.tracks[1].gain = 1.0;
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

    /* A text track goes into the visual band, which is in front of the audio
     * one in the list - so it moves the audio track's index. Everything below
     * looks tracks up by kind rather than by the number they used to be. */
    const int ti = sn::addTrack(p, sn::TRACK_TEXT);
    int vi = -1, ai = -1;
    for (size_t i = 0; i < p.tracks.size(); i++) {
        if (p.tracks[i].kind == sn::TRACK_VIDEO && vi < 0) vi = (int)i;
        if (p.tracks[i].kind == sn::TRACK_AUDIO && ai < 0) ai = (int)i;
    }
    CHECK(vi >= 0 && ai >= 0 && ti >= 0, "video, audio and text tracks to save");

    p.tracks[ai].clips[0].gain = 0.5;
    p.tracks[ai].gain = 0.75;
    p.tracks[vi].clips[0].fadeOut = 0.4;
    p.name = "a test";

    /* A caption, with something in every field that could be dropped. */
    sn::Clip cap;
    cap.id = p.newId();
    cap.in = 0.0;
    cap.out = 2.5;
    cap.pos = 1.25;
    cap.text.text = "two \\ lines\nand a backslash";
    cap.text.size = 0.123;
    cap.text.x = -0.4;
    cap.text.y = 0.6;
    cap.text.rotation = -7.5;
    cap.text.fill = 0x11223344u;
    cap.text.outline = 0x55667788u;
    cap.text.outlineWidth = 0.075;
    cap.text.align = 2;
    cap.text.lineSpacing = 1.4;
    sn::addClip(p, ti, cap);

    const std::string path = "media/test.bencsnip";
    CHECK(sn::saveProject(p, path, &err), "save: %s", err.c_str());

    sn::Project q;
    CHECK(sn::loadProject(&q, path, &err), "load: %s", err.c_str());
    CHECK(q.name == "a test", "the name came back, got '%s'", q.name.c_str());
    CHECK(q.bin.size() == p.bin.size(), "the bin came back");
    CHECK(q.tracks.size() == p.tracks.size(), "the tracks came back");
    CHECK(NEAR(q.duration(), p.duration(), 1e-6), "the duration is the same");
    CHECK(q.tracks[vi].clips.size() == p.tracks[vi].clips.size(), "the cuts came back");
    CHECK(NEAR(q.tracks[ai].clips[0].gain, 0.5, 1e-4), "so did the gain");
    CHECK(NEAR(q.tracks[ai].gain, 0.75, 1e-4), "and the track's own level");
    CHECK(NEAR(q.tracks[vi].gain, 1.0, 1e-9),
          "a track nobody touched comes back at unity, got %f", q.tracks[vi].gain);
    CHECK(NEAR(q.tracks[vi].clips[0].fadeOut, 0.4, 1e-4), "and the fade");
    CHECK(!q.dirty, "a project just loaded is not dirty");

    /* The caption, field by field. The string is the one that matters most:
     * it is the only thing in this format that is escaped, and a newline or a
     * backslash coming back wrong would be a project that silently changes
     * what it says. */
    CHECK(ti < (int)q.tracks.size() && q.tracks[ti].kind == sn::TRACK_TEXT,
          "the text track came back as a text track");
    if (ti < (int)q.tracks.size() && !q.tracks[ti].clips.empty()) {
        const sn::TextStyle &g = q.tracks[ti].clips[0].text;
        CHECK(g.text == cap.text.text, "the words came back: '%s'", g.text.c_str());
        CHECK(g.text.find('\n') != std::string::npos, "with the line break in them");
        CHECK(g.text.find('\\') != std::string::npos, "and the backslash");
        CHECK(NEAR(g.size, 0.123, 1e-6), "the size");
        CHECK(NEAR(g.x, -0.4, 1e-6) && NEAR(g.y, 0.6, 1e-6), "the position");
        CHECK(NEAR(g.rotation, -7.5, 1e-6), "the rotation");
        CHECK(g.fill == 0x11223344u && g.outline == 0x55667788u, "both colours");
        CHECK(NEAR(g.outlineWidth, 0.075, 1e-6), "the outline width");
        CHECK(g.align == 2, "the alignment");
        CHECK(NEAR(g.lineSpacing, 1.4, 1e-6), "the line spacing");
        CHECK(g == cap.text, "and nothing else drifted");
    } else {
        CHECK(false, "the caption did not come back at all");
    }

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

            /* The whole two seconds, to within half a frame.
             *
             * A GIF holds a delay per frame rather than a timestamp, so the
             * last frame's length has to be written rather than worked out
             * from the one after it. Nothing did, the muxer fell back to a
             * single centisecond, and every GIF this program wrote ended a
             * frame short - which on a loop is a blip on every pass. That
             * shows up here as a file that measures 1.9 seconds. */
            CHECK(NEAR(mi.duration, 2.0, 0.05),
                  "and lasts the full 2.00 s, got %.3f - a short last frame "
                  "blips on every loop", mi.duration);

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

/* ------------------------------------------------------------------ *
 * Text
 *
 * Needs no media and no ffmpeg: the font is compiled into the library, which
 * is the whole reason the embedded assets moved into the core. So this runs
 * on a machine with an empty media/ directory, which the rest of these do not.
 * ------------------------------------------------------------------ */

static void test_text()
{
    printf("text\n");

    const int W = 320, H = 180;
    std::vector<uint8_t> c((size_t)W * H * 4, 0);
    for (size_t i = 0; i < (size_t)W * H; i++) c[i * 4 + 3] = 255;

    auto lit = [&]() {
        int n = 0;
        for (size_t i = 0; i < (size_t)W * H; i++)
            if (c[i * 4] || c[i * 4 + 1] || c[i * 4 + 2]) n++;
        return n;
    };
    auto clear = [&]() {
        std::fill(c.begin(), c.end(), (uint8_t)0);
        for (size_t i = 0; i < (size_t)W * H; i++) c[i * 4 + 3] = 255;
    };

    /* Nothing to draw is false, and leaves the canvas alone. */
    sn::TextStyle none;
    CHECK(!sn::drawText(none, c.data(), W, H), "empty text draws nothing");
    CHECK(lit() == 0, "and puts no pixels down");

    /* Something to draw is true, and puts white where it said it would. */
    sn::TextStyle st;
    st.text = "Hi";
    st.size = 0.4;
    st.fill = 0xffffffffu;
    st.outlineWidth = 0.0;
    CHECK(sn::drawText(st, c.data(), W, H), "text draws");
    const int plain = lit();
    CHECK(plain > 0, "and lights pixels, got %d", plain);

    /* An outline is more pixels than no outline, and none of the extra ones
     * are the fill colour - which is what catches an outline drawn over the
     * letter instead of under it. */
    clear();
    sn::TextStyle out = st;
    out.outline = 0xff0000ffu;
    out.outlineWidth = 0.25;
    CHECK(sn::drawText(out, c.data(), W, H), "outlined text draws");
    const int outlined = lit();
    CHECK(outlined > plain, "an outline covers more, %d vs %d", outlined, plain);

    int red = 0, white = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        const uint8_t *p = &c[i * 4];
        if (p[0] > 200 && p[1] < 60 && p[2] < 60) red++;
        if (p[0] > 200 && p[1] > 200 && p[2] > 200) white++;
    }
    CHECK(red > 0, "the outline colour is on the canvas, got %d", red);
    CHECK(white > 0, "and the fill is still on top of it, got %d", white);

    /* Alpha fades it the way a clip's fade does. */
    clear();
    sn::drawText(st, c.data(), W, H, 0.25);
    int bright = 0;
    for (size_t i = 0; i < (size_t)W * H; i++)
        if (c[i * 4] > 200) bright++;
    CHECK(bright == 0, "a quarter alpha leaves nothing at full brightness, got %d", bright);
    CHECK(lit() > 0, "but it is still there");

    /* --- the box --- */
    double k[8], k2[8];
    CHECK(sn::textBox(st, W, H, k), "the box is reported");

    const double bw = k[2] - k[0], bh = k[5] - k[1];
    CHECK(bw > 0 && bh > 0, "and has a size, %f x %f", bw, bh);

    sn::TextStyle right = st;
    right.x = 0.9;
    CHECK(sn::textBox(right, W, H, k2), "the box moves with x");
    CHECK(k2[0] > k[0], "to the right, %f vs %f", k2[0], k[0]);

    /* Rotation tilts it: the top edge stops being level. */
    sn::TextStyle spun = st;
    spun.rotation = 30.0;
    CHECK(sn::textBox(spun, W, H, k2), "a rotated box is reported");
    CHECK(std::fabs(k2[3] - k2[1]) > 1.0, "and its top edge is not level, %f vs %f",
          k2[1], k2[3]);

    /* A font that is not there falls back and says so, rather than drawing
     * nothing and leaving somebody to wonder where the caption went. */
    sn::TextStyle gone = st;
    gone.font = "/definitely/not/a/font.ttf";
    clear();
    CHECK(sn::drawText(gone, c.data(), W, H), "a missing font still draws");
    CHECK(lit() > 0, "in the embedded face");
    CHECK(sn::textMissingFont(gone), "and reports that it fell back");
    CHECK(!sn::textMissingFont(st), "while the embedded face is not a fallback");

    /* Two lines are taller than one. */
    sn::TextStyle two = st;
    two.text = "Hi\nHi";
    double k3[8];
    CHECK(sn::textBox(two, W, H, k3), "a second line is measured");
    CHECK((k3[5] - k3[1]) > bh * 1.5, "and is most of twice as tall, %f vs %f",
          k3[5] - k3[1], bh);

    /* Whatever this machine has. Not required to be more than none - a
     * container with no fonts installed is a legal place to run the tests -
     * but everything it does report has to be usable. */
    const std::vector<sn::FontEntry> &fonts = sn::systemFonts();
    printf("  (%d system fonts on this machine)\n", (int)fonts.size());
    bool named = true;
    for (const sn::FontEntry &f : fonts)
        if (f.name.empty() || f.path.empty()) named = false;
    CHECK(named, "every font found has a name and a path");

    if (!fonts.empty()) {
        sn::TextStyle sys = st;
        sys.font = fonts[0].path;
        clear();
        CHECK(sn::drawText(sys, c.data(), W, H), "a system font draws");
        CHECK(!sn::textMissingFont(sys), "and is not a fallback: %s",
              fonts[0].name.c_str());
    }
}

int main()
{
    printf("BENCsnip core tests\n\n");

    test_timeline();
    test_text();
    test_loop();

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
