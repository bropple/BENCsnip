/*
 * BENCsnip - what the sound looks like. See sn_peaks.h.
 */

#include "sn_peaks.h"

#include "sn_media.h"

#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace sn {

namespace {

struct Job {
    int id = 0;
    std::string path;
};

struct Shop {
    std::thread th;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::map<int, std::shared_ptr<const Peaks>> done;
    bool quit = false;
    bool started = false;
};

Shop g;

/* One file, start to finish.
 *
 * The mixer's own path is used rather than a private decoder: whatever
 * audioAt does about sample rates, channel counts and resampling is what will
 * be played, and a waveform drawn from a different reading of the file is a
 * waveform that disagrees with what comes out of the speakers. */
void scan(const Job &j, Peaks *out)
{
    std::string err;
    Source *s = Source::open(j.path, &err);
    if (!s) return;

    if (!s->hasAudio()) {
        delete s;
        out->ready = true;          /* nothing to draw, and never ask again */
        return;
    }

    const double dur = s->info().duration;
    if (dur <= 0) { delete s; out->ready = true; return; }

    const int BLOCK = 4096;
    const double bucketSec = 1.0 / out->perSec;

    std::vector<float> buf((size_t)BLOCK * CHANS);
    out->hi.assign((size_t)std::ceil(dur * out->perSec) + 1, 0);

    double t = 0.0;
    while (t < dur) {
        {
            std::lock_guard<std::mutex> lk(g.lock);
            if (g.quit) break;
        }

        s->audioAt(t, BLOCK, buf.data());

        /* Every sample lands in the bucket its own moment belongs to, rather
         * than the whole block in the block's bucket: a block is 85 ms and a
         * bucket is 10, so the second way would draw a staircase. */
        for (int i = 0; i < BLOCK; i++) {
            const double st = t + i / (double)RATE;
            if (st >= dur) break;

            const size_t b = (size_t)(st / bucketSec);
            if (b >= out->hi.size()) break;

            float v = std::fabs(buf[(size_t)i * CHANS]);
            const float r = std::fabs(buf[(size_t)i * CHANS + 1]);
            if (r > v) v = r;

            const unsigned char q =
                (unsigned char)(v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f));
            if (q > out->hi[b]) out->hi[b] = q;
        }

        t += BLOCK / (double)RATE;
    }

    delete s;
    out->ready = true;
}

void worker()
{
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g.lock);
            g.wake.wait(lk, [] { return g.quit || !g.jobs.empty(); });
            if (g.quit) return;
            j = g.jobs.front();
            g.jobs.pop_front();
        }

        auto p = std::make_shared<Peaks>();
        scan(j, p.get());

        /* A new object and a pointer swap, never a write into the one the
         * GUI may be drawing from this very moment. */
        std::lock_guard<std::mutex> lk(g.lock);
        g.done[j.id] = std::move(p);
    }
}

} /* namespace */

void peaksAsk(int itemId, const std::string &path)
{
    std::lock_guard<std::mutex> lk(g.lock);
    if (g.done.count(itemId)) return;

    for (const Job &j : g.jobs)
        if (j.id == itemId) return;

    if (!g.started) {
        g.started = true;
        g.th = std::thread(worker);
    }

    /* An empty entry marks it as asked for, so a hundred frames of drawing
     * the same clip queue one job and not a hundred. It is replaced by the
     * real thing when the worker is done, and until then `ready` is false and
     * the clip draws flat. */
    g.done[itemId] = std::make_shared<Peaks>();
    g.jobs.push_back(Job{itemId, path});
    g.wake.notify_all();
}

std::shared_ptr<const Peaks> peaksGet(int itemId)
{
    std::lock_guard<std::mutex> lk(g.lock);
    auto it = g.done.find(itemId);
    return it == g.done.end() ? nullptr : it->second;
}

void peaksShutdown()
{
    {
        std::lock_guard<std::mutex> lk(g.lock);
        g.quit = true;
        g.wake.notify_all();
    }
    if (g.th.joinable()) g.th.join();

    std::lock_guard<std::mutex> lk(g.lock);
    g.done.clear();
    g.jobs.clear();
}

} /* namespace sn */
