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
    /* The open file for this bin item, and for this one of its channels.
     *
     * Keyed by both, because a Source asked for one channel is configured for
     * it: two clips playing two channels of one file need two decoders, the
     * same way the picture and the sound of one file already do. */
    Source *source(int itemId, int channel = -1);

    const Project *m_p = nullptr;
    std::map<int, Source *> m_open;
    std::map<int, bool> m_failed;
    std::string m_err;

    /* Scratch, kept between calls so a 60 Hz preview does not allocate a
     * megabyte per frame. */
    std::vector<uint8_t> m_canvas;
    std::vector<float> m_mix;

    /* One scaled picture per track, kept between frames rather than one
     * buffer used by each track in turn.
     *
     * It is what lets a source notice that the buffer it is being handed
     * already holds the frame being asked for - see VideoFrame::stamp - which
     * a single shared buffer would defeat, because every track would find the
     * previous track's picture in it. The cost is a canvas-sized buffer per
     * video track, which at a preview size is a few megabytes each. */
    std::map<int, VideoFrame> m_layers;

    /* One rasterised caption per text track, kept for the same reason and at
     * greater expense: building one lays out the glyphs and grows the outline
     * out of them, and a caption changes about twice an hour while the frame
     * it is drawn on changes thirty times a second.
     *
     * Keyed by track rather than by clip, because a track's clips do not
     * overlap - only one caption on it can be under the playhead - so this is
     * bounded by the number of text tracks rather than by the number of
     * captions in the project. Crossing the cut between two captions on one
     * track rebuilds, which is a rebuild that was going to happen anyway. */
    std::map<int, TextLayer> m_text;
};

/* What a track's effects lane puts its output at, at timeline time t, clamped
 * to 0..1. Shared by the mixer and the compositor, so a fade on a video track
 * and a fade on an audio track are the same shape and the same code. */
double fxGain(const Track &t, double at);

} /* namespace sn */

#endif /* SN_RENDER_H */
