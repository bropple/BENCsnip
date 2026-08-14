/*
 * BENCsnip - the timeline as pictures and sound
 *
 * One question, asked by two callers: what does the timeline look and sound
 * like at time t? The preview asks it sixty times a second for the frame under
 * the playhead; the exporter asks it once per output frame from beginning to
 * end. Answering it in one place is what keeps the exported file identical to
 * what the preview showed - the usual way that goes wrong is two pieces of
 * code that both know how fades work.
 *
 * A Renderer owns an open Source per bin item and is NOT thread-safe. The
 * player thread has one and the exporter has its own; a decoder is a read
 * position, and two threads sharing one is two threads seeking against each
 * other.
 */

#ifndef SN_RENDER_H
#define SN_RENDER_H

#include "sn_media.h"
#include "sn_timeline.h"

#include <map>
#include <string>
#include <vector>

namespace sn {

class Renderer {
public:
    explicit Renderer(const Project *p = nullptr) : m_p(p) {}
    ~Renderer();

    /* The project must outlive the Renderer, and must not be edited while a
     * call is in progress - the GUI copies the project for its player thread
     * for exactly this reason. */
    void setProject(const Project *p);

    /* Drop every open file. Call after a relink, or when the bin changes. */
    void reset();

    /* The composited picture at t, scaled to fit w x h with black bars where
     * the aspect does not match. Returns false only when nothing is under the
     * playhead at all - `out` is still a valid black frame, so a caller that
     * ignores the result draws the right thing. */
    bool videoAt(double t, int w, int h, VideoFrame *out);

    /* The mix at t, `frames` samples per channel, interleaved stereo. Always
     * writes the whole block; silence where there is nothing. */
    void audioAt(double t, int frames, float *dst);

    /* Whether any audio clip is under [t, t+dur) - lets the player skip the
     * mixer entirely on a video-only timeline. */
    bool hasAudioAt(double t, double dur) const;

private:
    Source *source(int itemId);

    const Project *m_p = nullptr;
    std::map<int, Source *> m_open;
    std::map<int, bool> m_failed;
    std::string m_err;

    /* Scratch, kept between calls so a 60 Hz preview does not allocate a
     * megabyte per frame. */
    std::vector<uint8_t> m_canvas;
    std::vector<float> m_mix;
    VideoFrame m_layer;
};

/* The multiplier a clip's fades put on it at timeline time t: 1 in the middle,
 * ramping at the ends. Shared by the mixer and the compositor so a video fade
 * and its audio fade are the same shape. */
double fadeGain(const Clip &c, double t);

} /* namespace sn */

#endif /* SN_RENDER_H */
