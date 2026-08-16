/*
 * BENCsnip - the media layer
 *
 * Everything that knows what a container, a codec or a pixel format is lives
 * behind this header. The rest of the program deals in seconds, RGBA and
 * interleaved stereo floats, and never learns what it was given.
 *
 * A Source opens a file twice - once for video, once for audio - and that is
 * deliberate. One AVFormatContext has one read position, so a seek made to
 * find a video frame also moves the audio, and playback of a clip whose audio
 * runs three seconds ahead of its video in the interleave turns into a
 * seek storm. Two contexts cost a file handle and some buffers, and in
 * exchange the two halves are simply independent.
 */

#ifndef SN_MEDIA_H
#define SN_MEDIA_H

#include <cstdint>
#include <string>
#include <vector>

namespace sn {

/* The project's internal audio format. Everything is resampled to this on the
 * way in and encoded from it on the way out, so a timeline holding a 44.1 kHz
 * mono voice memo and a 48 kHz 5.1 film mixes without any special case. */
enum {
    RATE = 48000,
    CHANS = 2
};

/* What a file turned out to be. Filled by probe() without decoding anything
 * beyond what the demuxer reads to find out. */
/* How long a still is, when nothing in the file says. Long enough to see and
 * short enough to be worth extending rather than always trimming. */
const double STILL_SECONDS = 5.0;

struct MediaInfo {
    std::string path;
    std::string name;         /* basename, for the bin                    */
    std::string container;    /* "QuickTime / MOV", for the info line     */

    double duration = 0.0;    /* seconds; 0 when the container won't say  */
    int64_t bytes = 0;

    bool hasVideo = false;
    bool hasAudio = false;

    int width = 0, height = 0;   /* coded size, before rotation           */
    double fps = 0.0;
    double sar = 1.0;            /* sample aspect: 1.0 unless anamorphic  */
    int rotation = 0;            /* 0/90/180/270, from the display matrix */
    std::string vcodec;

    int rate = 0, chans = 0;
    std::string acodec;

    /* The file had an audio stream and there was nothing in it.
     *
     * hasAudio is false in that case, because a track of digital silence is
     * not audio and putting it on the timeline gives somebody a clip to
     * wonder about, mute, and drag around for no reason. Phone video with the
     * microphone off, screen recordings, and anything converted from a GIF
     * all arrive this way. This flag is what lets the interface say which of
     * the two kinds of "no audio" a file is. */
    bool silentAudio = false;

    /* One picture and no sound: a png, a jpeg, a single-frame anything.
     *
     * A still has no duration of its own - a photograph is not five seconds
     * long - and a clip has to have a length before it can be put on a
     * timeline and trimmed, so `duration` is given the nominal one below.
     * The flag is what tells everything downstream that the number was
     * invented here rather than read out of the file, and it is why asking a
     * still for the frame at nine seconds gives you the picture rather than
     * nothing. */
    bool still = false;

    /* Display size: coded size with the pixel aspect and rotation applied.
     * This is the size a preview or an export should use. */
    int dispW() const;
    int dispH() const;
};

bool probe(const std::string &path, MediaInfo *out, std::string *err);

/* One decoded picture, RGBA8, already scaled and rotated for display. */
struct VideoFrame {
    int w = 0, h = 0;
    double pts = -1.0;             /* seconds from the start of the file */
    std::vector<uint8_t> rgba;

    /* Which conversion put these pixels here. Written only by Source, and
     * only so that a Source asked to fill the same buffer with the same frame
     * at the same size twice can see that it already has, and do nothing.
     *
     * Zero means nothing has filled it, which is what a fresh frame says. */
    uint64_t stamp = 0;

    bool valid() const { return w > 0 && h > 0 && !rgba.empty(); }
};

/* Interleaved stereo float at RATE. */
struct AudioBlock {
    double pts = -1.0;
    int frames = 0;                /* per channel                        */
    std::vector<float> pcm;        /* frames * CHANS                     */
};

class Source {
public:
    /* Returns null and fills err on a file libav cannot open. A file with
     * neither a video nor an audio stream is not an error here - the caller
     * decides whether that is useful - but a file that is not media at all
     * is. */
    /* `channel` is which of the file's audio channels this Source hands
     * back, or -1 for all of them mixed down to stereo the way libav would.
     *
     * A Source that has been asked for one channel is a different Source, not
     * a different call: a decoder is a read position and a resampler is a
     * configuration, and two clips wanting two channels of one file want two
     * of each. It is the same argument the video and audio halves of a file
     * already make - see the note at the top of this file. The channel comes
     * out on both output channels, because everything above this line is
     * stereo and a mono clip is a mono clip in both ears. */
    static Source *open(const std::string &path, std::string *err, int channel = -1);
    ~Source();

    const MediaInfo &info() const { return m_info; }

    bool hasVideo() const { return m_v.dec != nullptr; }
    bool hasAudio() const { return m_a.dec != nullptr; }

    /* The next picture in decode order, scaled to outW x outH. Passing 0 for
     * both keeps the source's display size. False at the end of the file. */
    bool nextVideo(VideoFrame *out, int outW = 0, int outH = 0);

    /* The picture visible at time t. Seeks to the keyframe at or before t and
     * decodes forward, except when t is only a little ahead of where the
     * decoder already is - scrubbing forward through a long GOP does not need
     * to start over from the keyframe every frame. */
    bool frameAt(double t, VideoFrame *out, int outW = 0, int outH = 0);

    /* Where the decoder currently sits, in source seconds. -1 before the
     * first frame comes out. */
    double videoPos() const { return m_vpos; }

    /* Whether the last converted frame can be trusted to be fully opaque -
     * that is, whether the format it was decoded from carries an alpha
     * channel at all. Most video does not, and a compositor that knows it is
     * laying down a solid picture can copy rows instead of reading, testing
     * and blending four channels a pixel at a time.
     *
     * False until something has been decoded, which is the careful answer:
     * treating a transparent layer as opaque paints over what is behind it. */
    bool videoOpaque() const { return m_opaque; }

    void seekVideo(double t);

    /* Audio from t onwards, resampled to RATE/CHANS. Call seekAudio once,
     * then nextAudio repeatedly; blocks come out in order and are however
     * long the source's frames happen to be. */
    void seekAudio(double t);
    bool nextAudio(AudioBlock *out);

    /* Exactly `frames` samples per channel starting at t, silence-padded past
     * the end of the file. This is what the mixer wants: a block per callback
     * at a known length, with no state to keep on the caller's side beyond
     * "the last t I asked for". */
    bool audioAt(double t, int frames, float *dst);

    /* One stream of one file: its own demuxer, its own decoder, its own read
     * position. Opaque pointers so nothing outside sn_media.cpp needs libav's
     * headers - public only because the file-local helpers that drive it take
     * one by reference. */
    struct Demux {
        void *fmt = nullptr;      /* AVFormatContext * */
        void *dec = nullptr;      /* AVCodecContext *  */
        void *pkt = nullptr;      /* AVPacket *        */
        void *frm = nullptr;      /* AVFrame *         */
        int index = -1;
        int draining = 0;
        int eof = 0;
        double tbase = 0.0;       /* av_q2d(stream time_base)             */
        int64_t start = 0;        /* stream start_time, 0 if unset        */

        /* Whether going back to the very beginning can be done by rewinding
         * the byte stream instead of asking libav to seek. See demux_seek:
         * true only for GIF, where it is both correct and the difference
         * between a loop that plays and a loop that hitches. */
        bool rewindable = false;
    };

private:
    Source() {}
    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;

    /* AVFrame * in, RGBA out: scale, colour-convert and apply the rotation
     * the container asked for. */
    bool convert(void *avframe, VideoFrame *out, int outW, int outH);

    MediaInfo m_info;
    Demux m_v, m_a;

    void *m_sws = nullptr;        /* SwsContext *                         */
    int m_swsW = 0, m_swsH = 0, m_swsFmt = -1, m_swsOutW = 0, m_swsOutH = 0;

    void *m_swr = nullptr;        /* SwrContext *                         */
    int m_swrRate = 0, m_swrFmt = -1, m_swrChans = 0;
    int m_channel = -1;         /* which channel this Source is for, or -1 */
    std::vector<float> m_native; /* the frame before one channel is picked  */

    double m_vpos = -1.0;

    /* Whether the frame in m_v.frm is still the one we last handed out.
     *
     * Kept, rather than unreffed the moment it has been converted, so that
     * asking for the same moment twice gives the same picture. Without it the
     * decoder is already sitting on that frame, so the second call decodes
     * the next one - and a paused preview walks forward a frame every time an
     * edit asks it to redraw. */
    bool m_vhave = false;

    /* How many frames this source has decoded, and what the last conversion
     * produced. Together they answer "does the buffer I have been handed
     * already hold this frame at this size?", and when it does the scale is
     * skipped entirely.
     *
     * That is not a rare case. A 30 fps clip on a 60 fps timeline is two
     * requests per frame, and at a 1600x900 preview a colour-convert and
     * scale is around four milliseconds - so the half of them that ask for a
     * picture already sitting in the buffer were most of what made a
     * non-native frame rate stutter.
     *
     * The count is the identity rather than the timestamp because a frame
     * without a usable pts leaves m_vpos where it was, and two different
     * pictures answering to the same number is exactly the mistake that shows
     * up as a preview that has stopped moving. */
    bool m_opaque = false;

    uint64_t m_frameNo = 0;
    uint64_t m_conv = 0;          /* the stamp we wrote into that buffer  */
    uint64_t m_convFrame = 0;     /* which decoded frame it holds         */
    int m_convW = 0, m_convH = 0;

    /* Audio held back between audioAt calls: whatever the last decoded frame
     * had left over after the mixer took its block. Without it every call
     * would discard the remainder of a 1024-sample frame, and the result is a
     * buzz at the block rate rather than the sound of the file. */
    std::vector<float> m_afifo;
    double m_afifoPts = -1.0;
    double m_awant = -1.0;        /* where audioAt expects to continue     */
};

/* A single picture from a file, for the bin's thumbnails. Opens, seeks to a
 * point that is likely to be something other than a black title card, decodes
 * one frame and closes. */
bool thumbnail(const std::string &path, int w, int h, VideoFrame *out);

/* Name of the encoder libav would use for a container, and whether a build of
 * ffmpeg has it at all - the export dialog greys out what is missing rather
 * than failing after the user has picked a filename. */
bool haveEncoder(const char *name);

/* Which libav this was built against, for the information window. Asked here
 * rather than in the GUI so that the one place including libav's headers stays
 * the one place. */
std::string libVersion();

/* "1920x1080", "23.976 fps", "3:20.5" - shared so the bin, the timeline and
 * the export dialog all say a duration the same way. */
std::string fmtTime(double seconds, bool withFrames = false, double fps = 0.0);
std::string fmtSize(int64_t bytes);

} /* namespace sn */

#endif /* SN_MEDIA_H */
