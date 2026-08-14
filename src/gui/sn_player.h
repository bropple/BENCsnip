/*
 * BENCsnip - playback
 *
 * A worker thread renders the timeline ahead of where it is being watched:
 * mixed audio into a ring buffer, composited pictures into a small queue. The
 * audio device drains the ring on its own thread, and how much it has drained
 * IS the clock - the picture is chosen to match it. Driving it the other way
 * round, from the wall clock or from frames drawn, is what makes a preview
 * that slowly slides out of sync with its own sound.
 *
 * The GUI never touches a decoder. It hands the player a copy of the project
 * and asks for the frame that should be on screen.
 */

#ifndef SN_PLAYER_H
#define SN_PLAYER_H

#include "sn_media.h"
#include "sn_render.h"
#include "sn_timeline.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace sn {

class Player {
public:
    Player();
    ~Player();

    /* The audio device has to exist first; main() owns it. */
    void start();
    void stop();

    /* The player keeps its own copy, so the GUI can go on editing while a
     * decode is in flight. Call after any edit; it is cheap enough to call
     * every frame and does nothing when nothing changed. */
    void setProject(const Project &p, uint64_t revision);

    /* What the preview is being drawn at. Smaller is faster, and a 4K source
     * previewed in a 700-pixel pane does not need to be decoded at 4K. */
    void setPreviewSize(int w, int h);

    void play();
    void pause();
    void togglePlay();
    bool playing() const { return m_playing.load(); }

    void seek(double t);
    double position() const;

    /* Where playback stops. Set to the end of the timeline by the caller. */
    void setEnd(double t) { m_end.store(t); }

    void setVolume(float v) { m_volume.store(v); }
    float volume() const { return m_volume.load(); }

    /* The picture that belongs on screen now. Returns false when there is
     * nothing new since the last call, and the caller keeps its texture. */
    bool takeFrame(VideoFrame *out);

    /* Peak level of what the device has most recently been given, for the
     * meter. Two channels. */
    void levels(float *l, float *r) const;

    /* Set while the worker is behind - the preview is dropping frames. The
     * status line says so rather than leaving the user to wonder why a 4K
     * timeline looks jerky. */
    bool struggling() const { return m_struggling.load(); }

private:
    void run();                          /* the worker                    */
    static void feed(void *buf, unsigned frames);   /* the audio callback */
    void fill(float *dst, int frames);

    /* --- shared with the audio thread --- */
    /* A single-producer single-consumer ring: the worker writes, the device
     * reads, and neither takes a lock. Sized at two seconds, which is far
     * more than a device buffer and enough that a slow seek on one clip does
     * not empty it. */
    enum { RING_FRAMES = RATE * 2 };
    std::vector<float> m_ring;
    std::atomic<uint64_t> m_wr{0}, m_rd{0};

    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_clockRun{false};
    std::atomic<double> m_clockBase{0.0};   /* timeline time of m_rd == base */
    std::atomic<uint64_t> m_rdBase{0};
    std::atomic<double> m_end{0.0};
    std::atomic<float> m_volume{1.0f};
    std::atomic<float> m_peakL{0.0f}, m_peakR{0.0f};
    std::atomic<bool> m_struggling{false};

    /* Set when the audio device would not open. The clock normally comes from
     * how much sound the device has drained, and on a machine with no sound
     * card there is none - so the worker moves the read cursor itself, from
     * the wall clock, and everything downstream is unchanged. */
    std::atomic<bool> m_noDevice{false};

    /* --- the worker's own --- */
    std::thread m_thread;
    mutable std::mutex m_lock;
    std::condition_variable m_wake;

    Project m_proj;                 /* the worker's copy      */
    uint64_t m_rev = 0;             /* which edit it reflects */
    bool m_projNew = false;

    double m_seekTo = -1.0;
    bool m_seekPending = false;

    int m_pw = 640, m_ph = 360;
    bool m_sizeNew = false;

    struct Queued {
        VideoFrame f;
        double t = 0;
    };
    std::deque<Queued> m_frames;
    VideoFrame m_out;               /* handed to the GUI      */
    bool m_outNew = false;

    double m_writeT = 0.0;          /* where the worker is mixing to      */
    double m_frameT = 0.0;          /* and rendering pictures for         */
};

} /* namespace sn */

#endif /* SN_PLAYER_H */
