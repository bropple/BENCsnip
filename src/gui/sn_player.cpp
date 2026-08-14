/*
 * BENCsnip - playback
 */

#include "sn_player.h"

#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace sn {

/* raylib's audio callback carries no user pointer, so the one player the
 * program has is reachable through this. A second Player would need the
 * callback to dispatch on the stream, and there is no second player. */
static Player *g_player = nullptr;
static AudioStream g_stream = {};
static bool g_streamOpen = false;

Player::Player()
{
    m_ring.assign(RING_FRAMES * CHANS, 0.0f);
}

Player::~Player()
{
    stop();
}

void Player::start()
{
    if (m_thread.joinable()) return;

    g_player = this;
    m_quit.store(false);

    /* Small enough that a pause takes effect immediately, large enough that
     * the callback is not the busiest thing on the machine. */
    SetAudioStreamBufferSizeDefault(1024);
    g_stream = LoadAudioStream(RATE, 32, CHANS);
    if (IsAudioDeviceReady() && g_stream.buffer) {
        g_streamOpen = true;
        SetAudioStreamCallback(g_stream, Player::feed);
        PlayAudioStream(g_stream);
    }
    /* No sound card, or a device that refused to open: the picture still has
     * to play. */
    m_noDevice.store(!g_streamOpen);

    m_thread = std::thread(&Player::run, this);
}

void Player::stop()
{
    if (!m_thread.joinable()) return;

    m_quit.store(true);
    m_wake.notify_all();
    m_thread.join();

    if (g_streamOpen) {
        StopAudioStream(g_stream);
        UnloadAudioStream(g_stream);
        g_streamOpen = false;
    }
    g_player = nullptr;
}

/* ------------------------------------------------------------------ *
 * The audio thread
 * ------------------------------------------------------------------ */

void Player::feed(void *buf, unsigned frames)
{
    Player *p = g_player;
    float *dst = (float *)buf;
    if (!p) {
        std::memset(dst, 0, (size_t)frames * CHANS * sizeof(float));
        return;
    }
    p->fill(dst, (int)frames);
}

void Player::fill(float *dst, int frames)
{
    const size_t need = (size_t)frames * CHANS;

    if (!m_clockRun.load()) {
        std::memset(dst, 0, need * sizeof(float));
        m_peakL.store(0.0f);
        m_peakR.store(0.0f);
        return;
    }

    const uint64_t wr = m_wr.load(std::memory_order_acquire);
    uint64_t rd = m_rd.load(std::memory_order_relaxed);

    const float vol = m_volume.load();
    float pl = 0, pr = 0;

    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        if (rd < wr) {
            const size_t o = (size_t)((rd % RING_FRAMES) * CHANS);
            l = m_ring[o] * vol;
            r = m_ring[o + 1] * vol;
            rd++;
        }
        /* An underrun writes silence and does NOT advance the read cursor:
         * the clock must not run ahead of sound that was never played, or the
         * picture jumps forward at every stall. */
        dst[i * CHANS + 0] = l;
        dst[i * CHANS + 1] = r;
        pl = std::max(pl, std::fabs(l));
        pr = std::max(pr, std::fabs(r));
    }

    m_rd.store(rd, std::memory_order_release);
    m_peakL.store(pl);
    m_peakR.store(pr);
}

/* ------------------------------------------------------------------ *
 * The GUI's side
 * ------------------------------------------------------------------ */

double Player::position() const
{
    const uint64_t rd = m_rd.load(std::memory_order_acquire);
    const uint64_t base = m_rdBase.load();
    const double t = m_clockBase.load() + (double)(rd - base) / RATE;
    return t < 0 ? 0 : t;
}

void Player::setProject(const Project &p, uint64_t revision)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (m_rev == revision) return;
    m_proj = p;
    m_rev = revision;
    m_projNew = true;
    m_wake.notify_all();
}

void Player::setPreviewSize(int w, int h)
{
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    std::lock_guard<std::mutex> g(m_lock);
    if (w == m_pw && h == m_ph) return;
    m_pw = w;
    m_ph = h;
    m_sizeNew = true;
    m_wake.notify_all();
}

void Player::play()
{
    if (m_playing.load()) return;
    m_playing.store(true);
    m_clockRun.store(true);
    std::lock_guard<std::mutex> g(m_lock);
    m_wake.notify_all();
}

void Player::pause()
{
    if (!m_playing.load()) return;

    /* Stopping the clock before anything else means the position the GUI
     * reads next is the one the user heard stop, not one a buffer's worth
     * ahead. */
    const double at = position();
    m_clockRun.store(false);
    m_playing.store(false);
    seek(at);
}

void Player::togglePlay()
{
    if (m_playing.load()) pause();
    else play();
}

void Player::seek(double t)
{
    if (t < 0) t = 0;
    std::lock_guard<std::mutex> g(m_lock);
    m_seekTo = t;
    m_seekPending = true;
    m_wake.notify_all();
}

bool Player::takeFrame(VideoFrame *out)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (!m_outNew) return false;
    *out = m_out;
    m_outNew = false;
    return true;
}

void Player::levels(float *l, float *r) const
{
    *l = m_peakL.load();
    *r = m_peakR.load();
}

/* ------------------------------------------------------------------ *
 * The worker
 * ------------------------------------------------------------------ */

void Player::run()
{
    Renderer ren;
    Project mine;
    int pw = 640, ph = 360;
    double fps = 30.0;
    bool have = false;

    /* The picture the queue is being filled from. Kept between iterations so
     * that ordinary playback is a straight walk forward through the file. */
    VideoFrame tmp;

    /* Where the still currently on screen was rendered from, or -1 when
     * there is none - the paused case re-renders only when this stops
     * matching the playhead. */
    double still = -1.0;

    /* Only used when there is no audio device - see m_noDevice. */
    std::chrono::steady_clock::time_point lastTick = std::chrono::steady_clock::now();
    double owed = 0.0;

    while (!m_quit.load()) {
        bool seeking = false;
        double seekTo = 0;

        if (m_noDevice.load()) {
            const auto now2 = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now2 - lastTick).count();
            lastTick = now2;
            if (m_clockRun.load() && m_playing.load()) {
                owed += dt * RATE;
                const uint64_t whole = (uint64_t)owed;
                if (whole) {
                    owed -= (double)whole;
                    m_rd.fetch_add(whole);
                    m_wr.store(m_rd.load());   /* nothing is being consumed */
                }
            } else {
                owed = 0;
            }
        }

        {
            std::unique_lock<std::mutex> g(m_lock);

            if (m_projNew) {
                mine = m_proj;
                m_projNew = false;
                have = true;
                fps = mine.fps > 0 ? mine.fps : 30.0;
                ren.setProject(&mine);

                /* An edit changes what should be on screen, so the queued
                 * pictures are thrown away and the paused still is redrawn.
                 *
                 * The audio already in the ring is NOT thrown away, and the
                 * clock is not touched. Dragging a clip sends a new project
                 * on every frame of the drag, and emptying the ring each time
                 * would mean silence for as long as the mouse is moving -
                 * where keeping it means at most half a second of sound from
                 * an arrangement that has since changed. */
                m_frames.clear();
                m_frameT = position();
                still = -1.0;
            }
            if (m_sizeNew) {
                pw = m_pw;
                ph = m_ph;
                m_sizeNew = false;
                m_frames.clear();
                still = -1.0;
            }
            if (m_seekPending) {
                seeking = true;
                seekTo = m_seekTo;
                m_seekPending = false;
            }
        }

        if (!have) {
            std::unique_lock<std::mutex> g(m_lock);
            m_wake.wait_for(g, std::chrono::milliseconds(20));
            continue;
        }

        if (seeking) {
            /* Stop the clock, empty everything, and restart from there. The
             * ring is not rewound - the cursors are moved to meet, which is
             * the same thing without touching memory the device may be
             * reading. */
            const bool wasRunning = m_playing.load();
            m_clockRun.store(false);

            std::unique_lock<std::mutex> g(m_lock);
            m_frames.clear();
            m_wr.store(m_rd.load());
            m_rdBase.store(m_rd.load());
            m_clockBase.store(seekTo);
            m_writeT = seekTo;
            m_frameT = seekTo;
            still = -1.0;
            g.unlock();

            if (wasRunning) m_clockRun.store(true);
        }

        const double now = position();
        const double end = m_end.load();

        /* Stop at the end rather than running on through silence. */
        if (m_playing.load() && end > 0 && now >= end) {
            m_playing.store(false);
            m_clockRun.store(false);
            std::unique_lock<std::mutex> g(m_lock);
            m_clockBase.store(end);
            m_rdBase.store(m_rd.load());
            m_writeT = end;
            m_frameT = end;
        }

        bool did = false;

        /* Decoding happens with no lock held. `mine` and the renderer belong
         * to this thread and nothing else reads them; holding the mutex
         * across a decode would stall the GUI for as long as a seek into a
         * long GOP takes, which is exactly when the GUI most needs to keep
         * drawing. */

        /* --- audio --- *
         * Skipped entirely with no device: mixing sound nobody can hear is
         * decoding work taken away from the picture, which is the only half
         * such a machine can show. */
        if (m_playing.load() && !m_noDevice.load()) {
            const int block = 1024;
            static thread_local std::vector<float> mix;
            mix.resize((size_t)block * CHANS);

            /* Keep about half a second in front of the device. More than that
             * and an edit takes visibly long to be heard; less and a slow
             * decode is audible as a gap. */
            const uint64_t ahead = m_wr.load() - m_rd.load();
            if (ahead + block < RATE / 2) {
                ren.audioAt(m_writeT, block, mix.data());

                uint64_t wr = m_wr.load();
                for (int i = 0; i < block; i++) {
                    const size_t o = (size_t)(((wr + i) % RING_FRAMES) * CHANS);
                    m_ring[o] = mix[i * CHANS];
                    m_ring[o + 1] = mix[i * CHANS + 1];
                }
                m_wr.store(wr + block, std::memory_order_release);
                m_writeT += block / (double)RATE;
                did = true;
            }
        }

        /* --- video --- */
        if (m_playing.load()) {
            std::unique_lock<std::mutex> g(m_lock);
            const size_t queued = m_frames.size();
            g.unlock();

            if (queued < 4 && m_frameT < now + 0.5) {
                const double t = m_frameT;
                ren.videoAt(t, pw, ph, &tmp);
                tmp.pts = t;

                Queued q;
                q.f = tmp;
                q.t = t;

                g.lock();
                m_frames.push_back(std::move(q));
                g.unlock();

                m_frameT = t + 1.0 / fps;
                still = -1.0;         /* the still is stale once we move */
                did = true;
            }

            /* Publish whatever the clock has reached, dropping anything it
             * has already gone past. */
            g.lock();
            int ready = 0;
            while (!m_frames.empty() && m_frames.front().t <= now + 1e-6) {
                m_out = std::move(m_frames.front().f);
                m_frames.pop_front();
                m_outNew = true;
                ready++;
            }
            g.unlock();

            /* More than one frame due at the same moment means the decode is
             * behind the clock. One is a hiccup; a steady stream of them is a
             * preview that cannot keep up, and the status line says so rather
             * than leaving the user to guess. */
            m_struggling.store(ready > 2);
        } else {
            /* Paused, or scrubbing. One picture, at wherever the playhead
             * is, and only when it has actually moved - otherwise this
             * re-decodes the same frame as fast as the machine can go. */
            if (still < 0 || std::fabs(still - now) > 1e-9) {
                ren.videoAt(now, pw, ph, &tmp);
                tmp.pts = now;

                std::unique_lock<std::mutex> g(m_lock);
                m_out = tmp;
                m_outNew = true;
                m_frames.clear();
                g.unlock();

                still = now;
                m_frameT = now;
                m_struggling.store(false);
                did = true;
            }
        }

        if (!did) {
            std::unique_lock<std::mutex> g(m_lock);
            m_wake.wait_for(g, std::chrono::milliseconds(4));
        }
    }
}

} /* namespace sn */
