/*
 * BENCsnip - what the sound looks like
 *
 * A waveform on a clip is not decoration. It is how somebody finds the word
 * to cut on, the gap between two takes, or the moment the music starts,
 * without playing the whole thing and guessing. The timeline drew a sine wave
 * before this, which looks like a waveform from across the room and tells you
 * nothing at all.
 *
 * Reading it means decoding every sample of the file, which is seconds of
 * work for a long one, so it happens on a worker thread and the clip draws
 * flat until the answer arrives. Nothing waits for it.
 *
 * What is kept is one byte per hundredth of a second: the loudest sample in
 * that window. A minute of audio is six kilobytes, an hour is a third of a
 * megabyte, and at any zoom the timeline can reach there are more buckets
 * than there are pixels to draw them in.
 */

#ifndef SN_PEAKS_H
#define SN_PEAKS_H

#include <memory>
#include <string>
#include <vector>

namespace sn {

struct Peaks {
    std::vector<unsigned char> hi;   /* loudest sample per bucket, 0..255 */
    double perSec = 100.0;           /* buckets per second of source      */
    bool ready = false;

    /* The loudest sample around a given moment of the *source*, 0..1.
     *
     * Asking in source time rather than timeline time is what makes a
     * trimmed, looped or reversed clip draw the right shape without any of
     * that being known here: the caller turns a pixel into a source time with
     * the same function the renderer uses to decide what to play. */
    float at(double srcTime) const
    {
        if (!ready || hi.empty() || srcTime < 0) return 0.0f;
        const size_t i = (size_t)(srcTime * perSec);
        return i < hi.size() ? hi[i] / 255.0f : 0.0f;
    }
};

/* Start reading this file's peaks if nobody has yet. Cheap to call every
 * frame, which is how the timeline uses it: a clip asks for what it needs
 * while drawing and takes whatever is there. */
void peaksAsk(int itemId, const std::string &path);

/* What has been read for this item, or null if it has not been asked for.
 *
 * Handed back as a shared pointer because the worker is writing on another
 * thread. It never modifies a Peaks that anyone can see: it builds a new one
 * and swaps the pointer, so a caller drawing from the old one keeps a
 * complete, unchanging picture until it lets go. Returning a raw pointer
 * would mean the vector could be replaced halfway through a draw. */
std::shared_ptr<const Peaks> peaksGet(int itemId);

void peaksShutdown();

} /* namespace sn */

#endif /* SN_PEAKS_H */
