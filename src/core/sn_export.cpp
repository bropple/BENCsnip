/*
 * BENCsnip - writing it out
 */

#include "sn_export.h"
#include "sn_gif.h"
#include "sn_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace sn {

static std::string averr2(int e)
{
    char b[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, b, sizeof b);
    return b;
}

/* The formats an encoder accepts. ffmpeg 7.1 replaced the arrays on AVCodec
 * with a query function and deprecated the fields; both spellings are here so
 * this builds against whatever the distribution ships.
 *
 * A macro rather than a function with a fallback argument, because naming the
 * deprecated field at all - even in an argument the new branch never uses -
 * is a warning per call site. */
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
template <typename T>
static const T *query_config(AVCodecContext *ctx, const AVCodec *codec, AVCodecConfig cfg)
{
    const void *list = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(ctx, codec, cfg, 0, &list, &n) >= 0 && list)
        return (const T *)list;
    return nullptr;
}
#define SN_SUPPORTED(T, ctx, codec, cfg, legacy) query_config<T>(ctx, codec, cfg)
#else
#define SN_SUPPORTED(T, ctx, codec, cfg, legacy) ((const T *)(codec)->legacy)
#endif

/* ------------------------------------------------------------------ *
 * The fast path
 * ------------------------------------------------------------------ */

/* Every clip on the timeline, flattened, for the checks below. */
static std::vector<const Clip *> all_clips(const Project &p)
{
    std::vector<const Clip *> v;
    for (const Track &t : p.tracks)
        for (const Clip &c : t.clips) v.push_back(&c);
    return v;
}

bool canStreamCopy(const Project &p, const ExportSettings &s, std::string *why)
{
    auto no = [&](const char *r) { if (why) *why = r; return false; };

    if (!s.allowCopy) return no("re-encoding was asked for");

    std::vector<const Clip *> cl = all_clips(p);
    if (cl.empty()) return no("there is nothing on the timeline");

    const int src = cl[0]->source;
    for (const Clip *c : cl) {
        if (c->source != src) return no("the timeline uses more than one file");
        if (std::fabs(c->in - cl[0]->in) > 1e-6 || std::fabs(c->out - cl[0]->out) > 1e-6 ||
            std::fabs(c->pos - cl[0]->pos) > 1e-6)
            return no("the clips have been cut apart");
        if (c->fadeIn > 0 || c->fadeOut > 0) return no("there is a fade on it");
        if (std::fabs(c->gain - 1.0) > 1e-6) return no("the volume was changed");
        if (c->muted) return no("something is muted");
    }
    if (cl.size() > 2) return no("more than one clip is on the timeline");

    const BinItem *b = p.item(src);
    if (!b || b->missing) return no("the file is missing");

    /* Everything the settings would change means re-encoding. */
    if (b->info.hasVideo) {
        if (s.width != b->info.dispW() || s.height != b->info.dispH())
            return no("the output size differs from the source");
        if (s.fps > 0 && b->info.fps > 0 && std::fabs(s.fps - b->info.fps) > 0.01)
            return no("the frame rate differs from the source");
    }

    /* The range asked for has to be inside the one clip. */
    double to = s.to < 0 ? p.duration() : s.to;
    if (s.from < cl[0]->pos - 1e-6 || to > cl[0]->end() + 1e-6)
        return no("the export range runs past the clip");

    /* And the container has to accept the codecs as they are. */
    const AVOutputFormat *of = av_guess_format(nullptr, s.path.c_str(), nullptr);
    if (!of) return no("the output format is unclear");

    if (why) *why = "";
    return true;
}

int64_t estimateSize(const Project &p, const ExportSettings &s, bool *exact)
{
    if (exact) *exact = false;

    const double to = s.to < 0 ? p.duration() : s.to;
    const double span = std::max(0.0, to - s.from);
    if (span <= 0) return 0;

    /* A copy writes the packets it reads, so the size is the source's own
     * bitrate over the range. Which is a division rather than a guess. */
    std::string why;
    if (canStreamCopy(p, s, &why)) {
        std::vector<const Clip *> cl = all_clips(p);
        const BinItem *b = cl.empty() ? nullptr : p.item(cl[0]->source);
        if (b && b->info.bytes > 0 && b->info.duration > 0) {
            if (exact) *exact = true;
            return (int64_t)(b->info.bytes * (span / b->info.duration));
        }
    }

    int64_t bits = 0;

    bool anyVideo = false, anyAudio = false;
    for (const Track &t : p.tracks)
        for (const Clip &c : t.clips) {
            const BinItem *b = p.item(c.source);
            if (!b) continue;
            if (t.kind == TRACK_VIDEO && b->info.hasVideo) anyVideo = true;
            if (t.kind == TRACK_AUDIO && b->info.hasAudio) anyAudio = true;
        }

    if (anyVideo && s.vcodec == "gif") {
        /* A GIF has no quality setting and no bitrate; it is one byte per
         * pixel through LZW, and what that compresses to depends entirely on
         * the footage. The test clips here come out around 0.5 bits per pixel
         * and dithered camera footage runs several times that, so 1.5 is a
         * figure between them rather than one measured from anything - which
         * is why this number is only ever shown after the word "about".
         *
         * It is here at all because a GIF is the one format where the answer
         * can be a hundred times what somebody expected: ten seconds of 1080p
         * is tens of megabytes, and it is better to see that in the dialog
         * than in the folder afterwards. */
        const double px = (double)s.width * s.height * (s.fps > 0 ? s.fps : 30.0);
        bits += (int64_t)(px * span * 1.5);
    } else if (anyVideo && !s.vcodec.empty() && s.vcodec != "none") {
        if (s.vbitrate > 0) {
            bits += (int64_t)(s.vbitrate * span);
        } else {
            /* Bits per pixel, at the quality asked for. crf 23 on ordinary
             * 1080p footage lands near 0.10 bpp; each six points of crf is
             * roughly a halving, which is the rule of thumb the encoder's own
             * documentation gives and is close enough for a number labelled
             * "about". */
            const double bpp = 0.10 * std::pow(2.0, (23.0 - s.crf) / 6.0);
            const double px = (double)s.width * s.height * (s.fps > 0 ? s.fps : 30.0);
            bits += (int64_t)(px * bpp * span);
        }
    }

    if (anyAudio && !s.acodec.empty() && s.acodec != "none") {
        /* pcm is not compressed and does not care what bitrate was asked
         * for: it is the sample rate times the width times the channels. */
        const bool pcm = s.acodec.compare(0, 3, "pcm") == 0;
        const int64_t rate = pcm ? (int64_t)RATE * 16 * CHANS : s.abitrate;
        bits += (int64_t)(rate * span);
    }

    /* Container overhead: a few per cent for mp4 and mkv, and it is better to
     * be a little over than a little under. */
    return (int64_t)(bits / 8.0 * 1.02);
}

static bool stream_copy(const Project &p, const ExportSettings &s, ExportStatus *st)
{
    std::vector<const Clip *> cl = all_clips(p);
    const Clip *c = cl[0];
    const BinItem *b = p.item(c->source);

    const double to = s.to < 0 ? p.duration() : s.to;
    const double srcFrom = c->srcAt(std::max(s.from, c->pos));
    const double srcTo = c->srcAt(std::min(to, c->end()));

    AVFormatContext *in = nullptr, *out = nullptr;
    int rc = avformat_open_input(&in, b->info.path.c_str(), nullptr, nullptr);
    if (rc < 0) { st->say("cannot open " + b->info.name); return false; }
    avformat_find_stream_info(in, nullptr);

    rc = avformat_alloc_output_context2(&out, nullptr, nullptr, s.path.c_str());
    if (rc < 0 || !out) { avformat_close_input(&in); st->say("cannot write that format"); return false; }

    std::vector<int> map(in->nb_streams, -1);
    for (unsigned i = 0; i < in->nb_streams; i++) {
        AVStream *is = in->streams[i];
        if (is->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            is->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (is->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;

        AVStream *os = avformat_new_stream(out, nullptr);
        if (!os) continue;
        avcodec_parameters_copy(os->codecpar, is->codecpar);
        os->codecpar->codec_tag = 0;
        os->time_base = is->time_base;
        map[i] = os->index;
    }

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&out->pb, s.path.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            st->say("cannot write " + s.path + ": " + averr2(rc));
            avformat_close_input(&in);
            avformat_free_context(out);
            return false;
        }
    }
    rc = avformat_write_header(out, nullptr);
    if (rc < 0) {
        st->say("this container will not hold those codecs as they are: " + averr2(rc));
        if (out->pb) avio_closep(&out->pb);
        avformat_close_input(&in);
        avformat_free_context(out);
        return false;
    }

    /* Seek to the keyframe at or before the in point. Everything between it
     * and the in point comes along - that is the deal a copy makes, and the
     * dialog says so. */
    int64_t seekTs = (int64_t)llround(srcFrom * AV_TIME_BASE);
    av_seek_frame(in, -1, seekTs, AVSEEK_FLAG_BACKWARD);

    /* Timestamps have to start near zero in the new file, and every stream
     * has to shift by the same amount or the sync goes. */
    std::vector<int64_t> off(in->nb_streams, AV_NOPTS_VALUE);

    AVPacket *pkt = av_packet_alloc();
    double lastT = srcFrom;

    while (av_read_frame(in, pkt) >= 0) {
        if (st->cancel.load()) { av_packet_unref(pkt); break; }

        int oi = pkt->stream_index < (int)map.size() ? map[pkt->stream_index] : -1;
        if (oi < 0) { av_packet_unref(pkt); continue; }

        AVStream *is = in->streams[pkt->stream_index];
        AVStream *os = out->streams[oi];

        double t = (pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts) * av_q2d(is->time_base);
        if (t > srcTo) { av_packet_unref(pkt); break; }

        if (off[pkt->stream_index] == AV_NOPTS_VALUE)
            off[pkt->stream_index] = (int64_t)llround(srcFrom / av_q2d(is->time_base));

        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= off[pkt->stream_index];
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= off[pkt->stream_index];

        /* A packet from before the in point has a negative timestamp now.
         * mp4 tolerates that (it becomes an edit list); others do not, so
         * shift such packets to zero rather than dropping them, which would
         * break the decoder's reference chain. */
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < 0) pkt->pts = 0;
        if (pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0) pkt->dts = 0;

        av_packet_rescale_ts(pkt, is->time_base, os->time_base);
        pkt->stream_index = oi;
        pkt->pos = -1;

        if (av_interleaved_write_frame(out, pkt) < 0) { av_packet_unref(pkt); break; }
        av_packet_unref(pkt);

        lastT = t;
        double span = srcTo - srcFrom;
        st->progress.store(span > 0 ? std::min(1.0, (lastT - srcFrom) / span) : 1.0);
    }

    av_write_trailer(out);
    av_packet_free(&pkt);
    if (out->pb) avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);

    st->progress.store(1.0);
    st->copied.store(true);
    return !st->cancel.load();
}

/* ------------------------------------------------------------------ *
 * The rendering path
 * ------------------------------------------------------------------ */

namespace {

struct VEnc {
    AVStream *st = nullptr;
    AVCodecContext *ctx = nullptr;
    AVFrame *frm = nullptr;
    SwsContext *sws = nullptr;
    int64_t n = 0;
};

struct AEnc {
    AVStream *st = nullptr;
    AVCodecContext *ctx = nullptr;
    AVFrame *frm = nullptr;
    SwrContext *swr = nullptr;
    AVAudioFifo *fifo = nullptr;
    int64_t n = 0;              /* input samples taken from the mixer, at RATE */
    int64_t out = 0;            /* samples handed to the encoder, at its rate  */
};

} /* namespace */

static bool write_packets(AVFormatContext *out, AVCodecContext *enc, AVStream *st,
                          AVPacket *pkt, ExportStatus *stt)
{
    for (;;) {
        int rc = avcodec_receive_packet(enc, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) { stt->say("encoder failed: " + averr2(rc)); return false; }

        /* How long this picture is on screen. Most encoders leave it at zero
         * and most containers do not care, because a player reads the time of
         * the next frame instead - but the last frame has no next one, and a
         * GIF stores a delay per frame rather than a timestamp.
         *
         * Left at zero the muxer falls back to a single centisecond, so every
         * GIF written here ended with a frame that flashed past in ten
         * milliseconds. On a loop that is a blip on every pass. One frame at
         * the encoder's own rate is exactly one tick of its time base. */
        if (pkt->duration == 0 && enc->codec_type == AVMEDIA_TYPE_VIDEO)
            pkt->duration = 1;

        av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
        pkt->stream_index = st->index;
        rc = av_interleaved_write_frame(out, pkt);
        av_packet_unref(pkt);
        if (rc < 0) { stt->say("cannot write: " + averr2(rc)); return false; }
    }
}

static bool render(const Project &p, const ExportSettings &s, ExportStatus *st)
{
    const double to = s.to < 0 ? p.duration() : s.to;
    const double span = to - s.from;
    if (span <= 0) { st->say("the export range is empty"); return false; }

    Renderer ren(&p);

    AVFormatContext *out = nullptr;
    int rc = avformat_alloc_output_context2(&out, nullptr, nullptr, s.path.c_str());
    if (rc < 0 || !out) { st->say("cannot write that format"); return false; }

    VEnc v;
    AEnc a;
    bool ok = true;

    /* Whether the timeline has anything of each kind, so an audio-only
     * timeline does not get a video track of black. */
    bool anyVideo = false, anyAudio = false;
    for (const Track &t : p.tracks)
        for (const Clip &c : t.clips) {
            const BinItem *b = p.item(c.source);
            if (!b) continue;
            if (t.kind == TRACK_VIDEO && b->info.hasVideo) anyVideo = true;
            if (t.kind == TRACK_AUDIO && b->info.hasAudio) anyAudio = true;
        }

    const bool wantV = !s.vcodec.empty() && s.vcodec != "none" && anyVideo;
    const bool wantA = !s.acodec.empty() && s.acodec != "none" && anyAudio;

    if (!wantV && !wantA) {
        st->say("there is nothing to write");
        avformat_free_context(out);
        return false;
    }

    /* --- video encoder --- */
    if (wantV) {
        const AVCodec *codec = avcodec_find_encoder_by_name(s.vcodec.c_str());
        if (!codec) { st->say("this build has no " + s.vcodec + " encoder"); ok = false; }

        if (ok) {
            v.st = avformat_new_stream(out, nullptr);
            v.ctx = avcodec_alloc_context3(codec);

            /* Encoders want even dimensions; a 1279-wide export is a failure
             * to open the encoder, ten seconds after the user chose a file. */
            v.ctx->width = std::max(2, s.width & ~1);
            v.ctx->height = std::max(2, s.height & ~1);

            AVRational fr = av_d2q(s.fps > 0 ? s.fps : 30.0, 1000000);
            v.ctx->time_base = av_inv_q(fr);
            v.ctx->framerate = fr;
            v.ctx->gop_size = (int)std::lround(std::max(1.0, s.fps)) * 2;
            v.ctx->max_b_frames = 2;
            v.ctx->pix_fmt = AV_PIX_FMT_YUV420P;

            const AVPixelFormat *pf = SN_SUPPORTED(AVPixelFormat, v.ctx, codec,
                                                   AV_CODEC_CONFIG_PIX_FORMAT, pix_fmts);
            if (pf) {
                bool has420 = false, hasPal = false;
                for (int i = 0; pf[i] != AV_PIX_FMT_NONE; i++) {
                    if (pf[i] == AV_PIX_FMT_YUV420P) has420 = true;
                    if (pf[i] == AV_PIX_FMT_PAL8) hasPal = true;
                }
                /* Taking whatever the encoder lists first is right for nearly
                 * everything and wrong for exactly one format that matters.
                 * The gif encoder lists rgb8 first - a fixed 3-3-2 palette,
                 * which is what made every GIF out of this program come back
                 * banded - and pal8, which is 256 colours somebody has to
                 * choose. Choosing them is the whole of sn_gif.cpp. */
                if (!has420) v.ctx->pix_fmt = hasPal ? AV_PIX_FMT_PAL8 : pf[0];
            }

            if (s.vbitrate > 0) v.ctx->bit_rate = s.vbitrate;
            if (out->oformat->flags & AVFMT_GLOBALHEADER)
                v.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            /* Not every encoder has these; the ones that do are the ones
             * anybody picks, and setting them on one that does not is a
             * no-op rather than an error. */
            if (s.vbitrate <= 0)
                av_opt_set_int(v.ctx->priv_data, "crf", s.crf, 0);
            av_opt_set(v.ctx->priv_data, "preset", s.preset.c_str(), 0);
            av_opt_set_int(v.ctx->priv_data, "cq", s.crf, 0);          /* nvenc  */
            av_opt_set_int(v.ctx->priv_data, "row-mt", 1, 0);          /* vpx    */

            rc = avcodec_open2(v.ctx, codec, nullptr);
            if (rc < 0) { st->say("cannot start " + s.vcodec + ": " + averr2(rc)); ok = false; }
        }

        if (ok) {
            avcodec_parameters_from_context(v.st->codecpar, v.ctx);
            v.st->time_base = v.ctx->time_base;

            v.frm = av_frame_alloc();
            v.frm->format = v.ctx->pix_fmt;
            v.frm->width = v.ctx->width;
            v.frm->height = v.ctx->height;
            if (av_frame_get_buffer(v.frm, 0) < 0) { st->say("out of memory"); ok = false; }

            /* swscale has no pal8 output - a palette is a decision, not a
             * conversion - so that one frame format is filled in by hand. */
            if (v.ctx->pix_fmt != AV_PIX_FMT_PAL8) {
                v.sws = sws_getContext(v.ctx->width, v.ctx->height, AV_PIX_FMT_RGBA,
                                       v.ctx->width, v.ctx->height, v.ctx->pix_fmt,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!v.sws) { st->say("cannot convert to the encoder's pixel format"); ok = false; }
            }
        }
    }

    /* --- audio encoder --- */
    if (ok && wantA) {
        const AVCodec *codec = avcodec_find_encoder_by_name(s.acodec.c_str());
        if (!codec) { st->say("this build has no " + s.acodec + " encoder"); ok = false; }

        if (ok) {
            a.st = avformat_new_stream(out, nullptr);
            a.ctx = avcodec_alloc_context3(codec);

            a.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            const AVSampleFormat *sf = SN_SUPPORTED(AVSampleFormat, a.ctx, codec,
                                                    AV_CODEC_CONFIG_SAMPLE_FORMAT, sample_fmts);
            if (sf) {
                bool ok2 = false;
                for (int i = 0; sf[i] != AV_SAMPLE_FMT_NONE; i++)
                    if (sf[i] == a.ctx->sample_fmt) ok2 = true;
                if (!ok2) a.ctx->sample_fmt = sf[0];
            }

            a.ctx->sample_rate = RATE;
            const int *sr = SN_SUPPORTED(int, a.ctx, codec, AV_CODEC_CONFIG_SAMPLE_RATE,
                                         supported_samplerates);
            if (sr) {
                bool ok2 = false;
                int best = 0;
                for (int i = 0; sr[i]; i++) {
                    if (sr[i] == RATE) ok2 = true;
                    if (sr[i] > best) best = sr[i];
                }
                if (!ok2) a.ctx->sample_rate = best ? best : RATE;
            }

            av_channel_layout_default(&a.ctx->ch_layout, CHANS);
            a.ctx->bit_rate = s.abitrate;
            a.ctx->time_base = AVRational{1, a.ctx->sample_rate};
            if (out->oformat->flags & AVFMT_GLOBALHEADER)
                a.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            rc = avcodec_open2(a.ctx, codec, nullptr);
            if (rc < 0) { st->say("cannot start " + s.acodec + ": " + averr2(rc)); ok = false; }
        }

        if (ok) {
            avcodec_parameters_from_context(a.st->codecpar, a.ctx);
            a.st->time_base = a.ctx->time_base;

            /* A codec with no fixed frame size (pcm, flac) takes whatever it
             * is given; one with a fixed size gets exactly that many. */
            int fs = a.ctx->frame_size > 0 ? a.ctx->frame_size : 1024;
            a.frm = av_frame_alloc();
            a.frm->format = a.ctx->sample_fmt;
            a.frm->sample_rate = a.ctx->sample_rate;
            av_channel_layout_copy(&a.frm->ch_layout, &a.ctx->ch_layout);
            a.frm->nb_samples = fs;
            if (av_frame_get_buffer(a.frm, 0) < 0) { st->say("out of memory"); ok = false; }

            AVChannelLayout inLayout;
            av_channel_layout_default(&inLayout, CHANS);
            rc = swr_alloc_set_opts2(&a.swr, &a.ctx->ch_layout, a.ctx->sample_fmt,
                                     a.ctx->sample_rate, &inLayout, AV_SAMPLE_FMT_FLT,
                                     RATE, 0, nullptr);
            if (rc < 0 || swr_init(a.swr) < 0) { st->say("cannot resample"); ok = false; }

            a.fifo = av_audio_fifo_alloc(a.ctx->sample_fmt, CHANS, fs * 4);
            if (!a.fifo) { st->say("out of memory"); ok = false; }
        }
    }

    /* --- open and write the header --- */
    if (ok && !(out->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&out->pb, s.path.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) { st->say("cannot write " + s.path + ": " + averr2(rc)); ok = false; }
    }
    if (ok) {
        rc = avformat_write_header(out, nullptr);
        if (rc < 0) { st->say("cannot start the file: " + averr2(rc)); ok = false; }
    }

    /* --- the palette, for the one format that needs one ---
     *
     * Before any of it is encoded, because the colours have to be chosen from
     * the whole export rather than from whichever frame happened to be first:
     * a clip that starts on black and ends in daylight would otherwise get a
     * palette of blacks and spend the rest of its length in the dark.
     *
     * Sixteen frames spread across the range, subsampled - the colours in a
     * frame do not change much between one and the next, and a palette built
     * from every pixel of every frame is the same palette for a hundred times
     * the work. */
    Palette pal;
    bool dither = false;

    if (ok && v.ctx && v.ctx->pix_fmt == AV_PIX_FMT_PAL8) {
        st->say("choosing colours");

        const int samples = 16;
        const int stepPx = std::max(1, (v.ctx->width * v.ctx->height) / 24000);

        std::vector<uint8_t> pool;
        VideoFrame look;

        for (int i = 0; i < samples && !st->cancel.load(); i++) {
            const double t = s.from + span * (i + 0.5) / samples;
            if (!ren.videoAt(t, v.ctx->width, v.ctx->height, &look)) continue;

            const size_t n = (size_t)look.w * look.h;
            for (size_t k = 0; k < n; k += stepPx) {
                const uint8_t *p = look.rgba.data() + k * 4;
                pool.insert(pool.end(), p, p + 4);
            }
        }

        buildPalette(pool.data(), pool.size() / 4, 256, &pal);

        /* Dither only when the palette had to approximate. A cartoon, a
         * screen recording or a title card usually has fewer than 256 colours
         * in it, the palette then holds every one of them exactly, and
         * dithering flat colour is how a clean GIF gets speckled and twice
         * the size. */
        dither = pal.n >= 256;
    }

    /* --- the loop --- */
    AVPacket *pkt = ok ? av_packet_alloc() : nullptr;
    VideoFrame pic;
    std::vector<float> mix;
    const int ablock = 1024;
    mix.resize((size_t)ablock * CHANS);

    bool wroteHeader = ok;

    while (ok && !st->cancel.load()) {
        /* Whichever stream is furthest behind goes next, and a stream that
         * has reached the end is infinitely far ahead - that is what keeps a
         * video that ends before its audio from writing frames past the end
         * of the export. */
        double vt = v.ctx ? s.from + v.n * av_q2d(v.ctx->time_base) : 1e18;
        double at = a.ctx ? s.from + a.n / (double)RATE : 1e18;
        if (vt >= to) vt = 1e18;
        if (at >= to) at = 1e18;

        if (vt > 1e17 && at > 1e17) break;

        if (v.ctx && vt <= at) {
            ren.videoAt(vt, v.ctx->width, v.ctx->height, &pic);

            if (av_frame_make_writable(v.frm) < 0) { ok = false; break; }

            if (v.sws) {
                const uint8_t *src[4] = {pic.rgba.data(), nullptr, nullptr, nullptr};
                int stride[4] = {v.ctx->width * 4, 0, 0, 0};
                sws_scale(v.sws, src, stride, 0, v.ctx->height, v.frm->data, v.frm->linesize);
            } else {
                /* pal8: an index per pixel in data[0], and the palette itself
                 * in data[1] as 256 words of ARGB. It goes in on every frame
                 * rather than once, because make_writable is entitled to hand
                 * back a different buffer whenever the encoder is still
                 * holding the last one. */
                quantise(pic.rgba.data(), v.ctx->width, v.ctx->height, pal,
                         v.frm->data[0], v.frm->linesize[0], dither);

                uint32_t *entries = (uint32_t *)v.frm->data[1];
                for (int i = 0; i < 256; i++) {
                    const uint8_t *c = pal.rgb[i < pal.n ? i : 0];
                    entries[i] = 0xff000000u | ((uint32_t)c[0] << 16) |
                                 ((uint32_t)c[1] << 8) | c[2];
                }
            }
            v.frm->pts = v.n++;

            rc = avcodec_send_frame(v.ctx, v.frm);
            if (rc < 0) { st->say("encoder refused a frame: " + averr2(rc)); ok = false; break; }
            if (!write_packets(out, v.ctx, v.st, pkt, st)) { ok = false; break; }
        } else if (a.ctx && at < 1e17) {
            ren.audioAt(at, ablock, mix.data());

            const uint8_t *in[1] = {(const uint8_t *)mix.data()};
            /* One conversion into a temporary frame, then into the fifo; the
             * encoder wants its own frame size and the mixer works in blocks
             * of 1024, and those are only the same number by luck. */
            AVFrame *tmp = av_frame_alloc();
            tmp->format = a.ctx->sample_fmt;
            tmp->sample_rate = a.ctx->sample_rate;
            av_channel_layout_copy(&tmp->ch_layout, &a.ctx->ch_layout);
            tmp->nb_samples = (int)av_rescale_rnd(swr_get_delay(a.swr, RATE) + ablock,
                                                  a.ctx->sample_rate, RATE, AV_ROUND_UP);
            if (av_frame_get_buffer(tmp, 0) < 0) { av_frame_free(&tmp); ok = false; break; }

            int got = swr_convert(a.swr, tmp->data, tmp->nb_samples, in, ablock);
            if (got > 0) av_audio_fifo_write(a.fifo, (void **)tmp->data, got);
            av_frame_free(&tmp);

            a.n += ablock;

            while (av_audio_fifo_size(a.fifo) >= a.frm->nb_samples) {
                if (av_frame_make_writable(a.frm) < 0) { ok = false; break; }
                av_audio_fifo_read(a.fifo, (void **)a.frm->data, a.frm->nb_samples);

                /* The encoder's time base is 1/sample_rate, so a frame's pts
                 * is simply how many samples went in before it. Counting the
                 * mixer's blocks instead would drift whenever the encoder
                 * runs at something other than 48 kHz. */
                a.frm->pts = a.out;
                a.out += a.frm->nb_samples;

                rc = avcodec_send_frame(a.ctx, a.frm);
                if (rc < 0) { st->say("encoder refused audio: " + averr2(rc)); ok = false; break; }
                if (!write_packets(out, a.ctx, a.st, pkt, st)) { ok = false; break; }
            }
        } else {
            break;
        }

        double done = std::min(vt, at) - s.from;
        if (done < 1e17) st->progress.store(std::max(0.0, std::min(1.0, done / span)));
    }

    /* --- flush --- */
    if (ok && !st->cancel.load()) {
        if (a.ctx) {
            /* Whatever is left in the fifo, padded out to a frame. */
            if (av_audio_fifo_size(a.fifo) > 0) {
                int left = av_audio_fifo_size(a.fifo);
                if (av_frame_make_writable(a.frm) == 0) {
                    /* Silence first, then the remainder over the top: a codec
                     * with a fixed frame size gets a full frame either way,
                     * and the tail is silence rather than whatever was in the
                     * buffer last time round. */
                    av_samples_set_silence(a.frm->data, 0, a.frm->nb_samples, CHANS,
                                           a.ctx->sample_fmt);
                    av_audio_fifo_read(a.fifo, (void **)a.frm->data, left);
                    a.frm->pts = a.out;
                    a.out += a.frm->nb_samples;
                    avcodec_send_frame(a.ctx, a.frm);
                    write_packets(out, a.ctx, a.st, pkt, st);
                }
            }
            avcodec_send_frame(a.ctx, nullptr);
            write_packets(out, a.ctx, a.st, pkt, st);
        }
        if (v.ctx) {
            avcodec_send_frame(v.ctx, nullptr);
            write_packets(out, v.ctx, v.st, pkt, st);
        }
    }

    if (wroteHeader) av_write_trailer(out);

    /* --- teardown --- */
    if (pkt) av_packet_free(&pkt);
    if (v.sws) sws_freeContext(v.sws);
    if (v.frm) av_frame_free(&v.frm);
    if (v.ctx) avcodec_free_context(&v.ctx);
    if (a.fifo) av_audio_fifo_free(a.fifo);
    if (a.swr) swr_free(&a.swr);
    if (a.frm) av_frame_free(&a.frm);
    if (a.ctx) avcodec_free_context(&a.ctx);
    if (out->pb) avio_closep(&out->pb);
    avformat_free_context(out);

    if (ok && !st->cancel.load()) st->progress.store(1.0);
    return ok && !st->cancel.load();
}

/* ------------------------------------------------------------------ *
 * exportTimeline
 * ------------------------------------------------------------------ */

bool exportTimeline(const Project &p, const ExportSettings &s, ExportStatus *st)
{
    st->running.store(true);
    st->ok.store(false);
    st->copied.store(false);
    st->progress.store(0.0);

    bool result;
    std::string why;
    if (canStreamCopy(p, s, &why)) {
        st->say("copying without re-encoding");
        result = stream_copy(p, s, st);
        /* A container that turns out not to accept the codecs is not a
         * failure the user should have to understand - fall back and render
         * it, which always works. */
        if (!result && !st->cancel.load()) {
            st->copied.store(false);
            st->say("re-encoding");
            result = render(p, s, st);
        }
    } else {
        st->say("rendering");
        result = render(p, s, st);
    }

    if (result) st->say("done");
    else if (st->cancel.load()) st->say("cancelled");

    st->ok.store(result);
    st->running.store(false);
    return result;
}

} /* namespace sn */
