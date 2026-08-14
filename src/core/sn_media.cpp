/*
 * BENCsnip - the media layer
 *
 * See sn_media.h for why a Source opens the same file twice.
 */

#include "sn_media.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace sn {

/* ------------------------------------------------------------------ *
 * Small shared things
 * ------------------------------------------------------------------ */

static void quiet_once()
{
    static bool done = false;
    if (!done) {
        /* libav writes to stderr by default, and a file with a slightly
         * broken header produces a screenful of it. The GUI has nowhere to
         * show that, and a warning about a non-conforming SPS is not
         * something anyone dragging a phone video into an editor can act on.
         * Errors still come through, and probe() reports the ones that
         * matter as text. */
        av_log_set_level(AV_LOG_ERROR);
        done = true;
    }
}

static std::string averr(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, buf, sizeof buf);
    return std::string(buf);
}

static std::string basename_of(const std::string &p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string fmtSize(int64_t bytes)
{
    char b[64];
    const double k = 1024.0;
    if (bytes < 1024) snprintf(b, sizeof b, "%lld B", (long long)bytes);
    else if (bytes < 1024LL * 1024) snprintf(b, sizeof b, "%.0f KB", bytes / k);
    else if (bytes < 1024LL * 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", bytes / (k * k));
    else snprintf(b, sizeof b, "%.2f GB", bytes / (k * k * k));
    return b;
}

std::string fmtTime(double s, bool withFrames, double fps)
{
    if (s < 0) s = 0;
    int total = (int)s;
    int h = total / 3600, m = (total / 60) % 60, sec = total % 60;
    char b[64];

    if (withFrames && fps > 0) {
        int f = (int)((s - total) * fps + 1e-6);
        if (h) snprintf(b, sizeof b, "%d:%02d:%02d:%02d", h, m, sec, f);
        else snprintf(b, sizeof b, "%02d:%02d:%02d", m, sec, f);
    } else {
        int cs = (int)((s - total) * 100.0 + 1e-6);
        if (h) snprintf(b, sizeof b, "%d:%02d:%02d.%02d", h, m, sec, cs);
        else snprintf(b, sizeof b, "%02d:%02d.%02d", m, sec, cs);
    }
    return b;
}

std::string libVersion()
{
    char b[128];
    snprintf(b, sizeof b, "libavformat %d.%d.%d", LIBAVFORMAT_VERSION_MAJOR,
             LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO);
    return b;
}

bool haveEncoder(const char *name)
{
    quiet_once();
    return avcodec_find_encoder_by_name(name) != nullptr;
}

int MediaInfo::dispW() const
{
    int w = width;
    if (sar > 0.0 && std::fabs(sar - 1.0) > 0.001) w = (int)std::lround(width * sar);
    return (rotation == 90 || rotation == 270) ? height : w;
}

int MediaInfo::dispH() const
{
    int w = width;
    if (sar > 0.0 && std::fabs(sar - 1.0) > 0.001) w = (int)std::lround(width * sar);
    return (rotation == 90 || rotation == 270) ? w : height;
}

/* The display matrix, as the number of degrees clockwise a player must turn
 * the picture. Phones write 90 here and nothing else in the file says so;
 * ignoring it is why so many tools show portrait video on its side. */
static int stream_rotation(AVStream *st)
{
    const int32_t *mat = nullptr;

#if LIBAVCODEC_VERSION_MAJOR >= 61
    const AVPacketSideData *sd = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (sd && sd->size >= 9 * sizeof(int32_t)) mat = (const int32_t *)sd->data;
#else
    size_t sz = 0;
    const uint8_t *p = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, &sz);
    if (p && sz >= 9 * sizeof(int32_t)) mat = (const int32_t *)p;
#endif
    if (!mat) return 0;

    double deg = -av_display_rotation_get(mat);
    if (std::isnan(deg)) return 0;
    int r = (int)std::lround(deg / 90.0) * 90;
    r %= 360;
    if (r < 0) r += 360;
    return r;
}

static double stream_fps(AVStream *st)
{
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
        return av_q2d(st->avg_frame_rate);
    if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
        return av_q2d(st->r_frame_rate);
    return 0.0;
}

/* ------------------------------------------------------------------ *
 * probe
 * ------------------------------------------------------------------ */

bool probe(const std::string &path, MediaInfo *out, std::string *err)
{
    quiet_once();

    AVFormatContext *fmt = nullptr;
    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        if (err) *err = basename_of(path) + ": " + averr(rc);
        return false;
    }
    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        if (err) *err = basename_of(path) + ": no streams (" + averr(rc) + ")";
        avformat_close_input(&fmt);
        return false;
    }

    MediaInfo mi;
    mi.path = path;
    mi.name = basename_of(path);
    mi.container = fmt->iformat->long_name ? fmt->iformat->long_name
                                           : (fmt->iformat->name ? fmt->iformat->name : "?");
    if (fmt->duration != AV_NOPTS_VALUE) mi.duration = fmt->duration / (double)AV_TIME_BASE;
    if (fmt->pb) mi.bytes = avio_size(fmt->pb);

    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    /* An mp3 with cover art has a video stream of one still picture. Calling
     * that a video would put a track on the timeline that shows one frame for
     * four minutes, so it is treated as what it is: artwork on an audio
     * file. */
    if (vi >= 0 && (fmt->streams[vi]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        vi = -1;

    if (vi >= 0) {
        AVStream *st = fmt->streams[vi];
        const AVCodecDescriptor *cd = avcodec_descriptor_get(st->codecpar->codec_id);
        mi.hasVideo = true;
        mi.width = st->codecpar->width;
        mi.height = st->codecpar->height;
        mi.fps = stream_fps(st);
        mi.rotation = stream_rotation(st);
        if (st->codecpar->sample_aspect_ratio.num > 0 &&
            st->codecpar->sample_aspect_ratio.den > 0)
            mi.sar = av_q2d(st->codecpar->sample_aspect_ratio);
        mi.vcodec = cd ? cd->name : "?";
        if (mi.duration <= 0 && st->duration != AV_NOPTS_VALUE)
            mi.duration = st->duration * av_q2d(st->time_base);
    }
    if (ai >= 0) {
        AVStream *st = fmt->streams[ai];
        const AVCodecDescriptor *cd = avcodec_descriptor_get(st->codecpar->codec_id);
        mi.hasAudio = true;
        mi.rate = st->codecpar->sample_rate;
        mi.chans = st->codecpar->ch_layout.nb_channels;
        mi.acodec = cd ? cd->name : "?";
        if (mi.duration <= 0 && st->duration != AV_NOPTS_VALUE)
            mi.duration = st->duration * av_q2d(st->time_base);
    }

    avformat_close_input(&fmt);

    if (!mi.hasVideo && !mi.hasAudio) {
        if (err) *err = mi.name + ": no video or audio in it";
        return false;
    }
    if (mi.duration < 0) mi.duration = 0;
    *out = mi;
    return true;
}

/* ------------------------------------------------------------------ *
 * Source
 * ------------------------------------------------------------------ */

#define FMT(d)  ((AVFormatContext *)(d).fmt)
#define DEC(d)  ((AVCodecContext *)(d).dec)
#define PKT(d)  ((AVPacket *)(d).pkt)
#define FRM(d)  ((AVFrame *)(d).frm)

static void demux_close(Source::Demux &d)
{
    if (d.frm) { AVFrame *f = (AVFrame *)d.frm; av_frame_free(&f); d.frm = nullptr; }
    if (d.pkt) { AVPacket *p = (AVPacket *)d.pkt; av_packet_free(&p); d.pkt = nullptr; }
    if (d.dec) { AVCodecContext *c = (AVCodecContext *)d.dec; avcodec_free_context(&c); d.dec = nullptr; }
    if (d.fmt) { AVFormatContext *f = (AVFormatContext *)d.fmt; avformat_close_input(&f); d.fmt = nullptr; }
    d.index = -1;
}

static bool demux_open(const std::string &path, AVMediaType type, Source::Demux &d)
{
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return false; }

    const AVCodec *codec = nullptr;
    int idx = av_find_best_stream(fmt, type, -1, -1, &codec, 0);
    if (idx < 0 || !codec ||
        (type == AVMEDIA_TYPE_VIDEO &&
         (fmt->streams[idx]->disposition & AV_DISPOSITION_ATTACHED_PIC))) {
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) { avformat_close_input(&fmt); return false; }
    avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);

    /* Frame threading on the video decoder is most of the difference between
     * scrubbing that keeps up and scrubbing that does not, and libav picks a
     * sensible count from the machine when this is left at zero. */
    dec->thread_count = 0;
    dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    dec->pkt_timebase = fmt->streams[idx]->time_base;

    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }

    d.fmt = fmt;
    d.dec = dec;
    d.index = idx;
    d.pkt = av_packet_alloc();
    d.frm = av_frame_alloc();
    d.tbase = av_q2d(fmt->streams[idx]->time_base);
    d.start = fmt->streams[idx]->start_time == AV_NOPTS_VALUE
                  ? 0 : fmt->streams[idx]->start_time;
    return true;
}

Source *Source::open(const std::string &path, std::string *err)
{
    quiet_once();

    MediaInfo mi;
    if (!probe(path, &mi, err)) return nullptr;

    Source *s = new Source();
    s->m_info = mi;

    if (mi.hasVideo && !demux_open(path, AVMEDIA_TYPE_VIDEO, s->m_v))
        s->m_info.hasVideo = false;
    if (mi.hasAudio && !demux_open(path, AVMEDIA_TYPE_AUDIO, s->m_a))
        s->m_info.hasAudio = false;

    if (!s->hasVideo() && !s->hasAudio()) {
        if (err) *err = mi.name + ": nothing in it could be decoded";
        delete s;
        return nullptr;
    }
    return s;
}

Source::~Source()
{
    demux_close(m_v);
    demux_close(m_a);
    if (m_sws) { SwsContext *c = (SwsContext *)m_sws; sws_freeContext(c); }
    if (m_swr) { SwrContext *c = (SwrContext *)m_swr; swr_free(&c); }
}

/* Pull one frame out of a decoder, feeding it packets as it asks for them.
 * Returns 1 on a frame, 0 at the end of the stream. */
static int demux_next_frame(Source::Demux &d)
{
    if (!d.dec) return 0;

    for (;;) {
        int rc = avcodec_receive_frame(DEC(d), FRM(d));
        if (rc == 0) return 1;
        if (rc == AVERROR_EOF) { d.eof = 1; return 0; }
        if (rc != AVERROR(EAGAIN)) return 0;

        if (d.draining) return 0;

        /* Feed it. Packets from other streams in the same file are not our
         * business - this context exists for one stream. */
        int got = 0;
        for (;;) {
            rc = av_read_frame(FMT(d), PKT(d));
            if (rc < 0) break;
            if (PKT(d)->stream_index == d.index) { got = 1; break; }
            av_packet_unref(PKT(d));
        }

        if (got) {
            rc = avcodec_send_packet(DEC(d), PKT(d));
            av_packet_unref(PKT(d));
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                /* A corrupt packet in the middle of a file is common and
                 * survivable: drop it and keep going rather than declaring
                 * the clip over. */
                continue;
            }
        } else {
            avcodec_send_packet(DEC(d), nullptr);
            d.draining = 1;
        }
    }
}

static double frame_time(const Source::Demux &d, const AVFrame *f)
{
    int64_t ts = f->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = f->pts;
    if (ts == AV_NOPTS_VALUE) return -1.0;
    return (ts - d.start) * d.tbase;
}

static void demux_seek(Source::Demux &d, double t)
{
    if (!d.dec) return;
    if (t < 0) t = 0;

    int64_t ts = (int64_t)llround(t / d.tbase) + d.start;
    if (avformat_seek_file(FMT(d), d.index, INT64_MIN, ts, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(FMT(d), d.index, ts, AVSEEK_FLAG_BACKWARD);

    avcodec_flush_buffers(DEC(d));
    d.draining = 0;
    d.eof = 0;
}

/* Scale, convert and rotate one decoded picture into RGBA. */
bool Source::convert(void *avframe, VideoFrame *out, int outW, int outH)
{
    AVFrame *f = (AVFrame *)avframe;
    if (f->width <= 0 || f->height <= 0) return false;

    if (outW <= 0 || outH <= 0) {
        outW = m_info.dispW();
        outH = m_info.dispH();
    }
    if (outW <= 0 || outH <= 0) { outW = f->width; outH = f->height; }

    const bool swap = (m_info.rotation == 90 || m_info.rotation == 270);
    const int sw = swap ? outH : outW;   /* what sws produces, pre-rotation */
    const int sh = swap ? outW : outH;

    SwsContext *sws = (SwsContext *)m_sws;
    if (!sws || m_swsW != f->width || m_swsH != f->height || m_swsFmt != f->format ||
        m_swsOutW != sw || m_swsOutH != sh) {
        sws = sws_getCachedContext((SwsContext *)m_sws, f->width, f->height,
                                   (AVPixelFormat)f->format, sw, sh,
                                   AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr);
        if (!sws) return false;
        m_sws = sws;
        m_swsW = f->width; m_swsH = f->height; m_swsFmt = f->format;
        m_swsOutW = sw; m_swsOutH = sh;
    }

    std::vector<uint8_t> tmp;
    uint8_t *dstBuf;
    int dstStride = sw * 4;

    if (swap) {
        tmp.resize((size_t)sw * sh * 4);
        dstBuf = tmp.data();
    } else {
        out->rgba.resize((size_t)outW * outH * 4);
        dstBuf = out->rgba.data();
    }

    uint8_t *dst[4] = {dstBuf, nullptr, nullptr, nullptr};
    int stride[4] = {dstStride, 0, 0, 0};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);

    if (swap || m_info.rotation == 180) {
        if (!swap) { tmp.assign(out->rgba.begin(), out->rgba.end()); }
        out->rgba.resize((size_t)outW * outH * 4);
        const uint32_t *src = (const uint32_t *)tmp.data();
        uint32_t *d2 = (uint32_t *)out->rgba.data();
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                int sx, sy;
                switch (m_info.rotation) {
                case 90:  sx = y;              sy = sh - 1 - x;      break;
                case 180: sx = sw - 1 - x;     sy = sh - 1 - y;      break;
                default:  sx = sw - 1 - y;     sy = x;               break; /* 270 */
                }
                d2[(size_t)y * outW + x] = src[(size_t)sy * sw + sx];
            }
        }
    }

    out->w = outW;
    out->h = outH;
    return true;
}

bool Source::nextVideo(VideoFrame *out, int outW, int outH)
{
    if (!hasVideo()) return false;
    if (!demux_next_frame(m_v)) return false;

    double t = frame_time(m_v, FRM(m_v));
    if (t >= 0) m_vpos = t;
    out->pts = m_vpos;

    bool ok = convert(m_v.frm, out, outW, outH);
    av_frame_unref(FRM(m_v));
    return ok;
}

void Source::seekVideo(double t)
{
    demux_seek(m_v, t);
    m_vpos = -1.0;
}

bool Source::frameAt(double t, VideoFrame *out, int outW, int outH)
{
    if (!hasVideo()) return false;
    if (t < 0) t = 0;

    /* Scrubbing forward a little should not start over from the keyframe.
     * Two seconds is longer than most GOPs and much shorter than the time a
     * seek plus a re-decode costs, so decoding through is the cheaper answer
     * inside that window. */
    const double gap = t - m_vpos;
    if (m_vpos < 0.0 || gap < -0.001 || gap > 2.0) seekVideo(t);

    const double eps = m_info.fps > 0 ? 0.5 / m_info.fps : 0.02;

    bool haveAny = false;
    for (;;) {
        if (!demux_next_frame(m_v)) break;

        double ft = frame_time(m_v, FRM(m_v));
        if (ft >= 0) m_vpos = ft;
        haveAny = true;

        if (m_vpos >= t - eps) {
            out->pts = m_vpos;
            bool ok = convert(m_v.frm, out, outW, outH);
            av_frame_unref(FRM(m_v));
            return ok;
        }

        /* Not there yet. Keep the last one anyway: at the end of a file
         * every remaining frame is behind t, and showing the final picture
         * beats showing nothing. */
        if (m_v.eof || m_v.draining) {
            out->pts = m_vpos;
            bool ok = convert(m_v.frm, out, outW, outH);
            av_frame_unref(FRM(m_v));
            return ok;
        }
        av_frame_unref(FRM(m_v));
    }

    return haveAny ? false : false;
}

/* ------------------------------------------------------------------ *
 * Audio
 * ------------------------------------------------------------------ */

void Source::seekAudio(double t)
{
    demux_seek(m_a, t);
    m_afifo.clear();
    m_afifoPts = -1.0;
    m_awant = t;
}

bool Source::nextAudio(AudioBlock *out)
{
    if (!hasAudio()) return false;
    if (!demux_next_frame(m_a)) return false;

    AVFrame *f = FRM(m_a);
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, CHANS);

    SwrContext *swr = (SwrContext *)m_swr;
    if (!swr) {
        int rc = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, RATE,
                                     &f->ch_layout, (AVSampleFormat)f->format,
                                     f->sample_rate, 0, nullptr);
        if (rc < 0 || swr_init(swr) < 0) {
            if (swr) swr_free(&swr);
            av_frame_unref(f);
            return false;
        }
        m_swr = swr;
        m_swrRate = f->sample_rate;
        m_swrFmt = f->format;
        m_swrChans = f->ch_layout.nb_channels;
    } else if (m_swrRate != f->sample_rate || m_swrFmt != f->format ||
               m_swrChans != f->ch_layout.nb_channels) {
        /* Some containers change format mid-stream. Rebuilding is rare
         * enough that the cost does not matter and cheap enough that
         * refusing to would be silly. */
        swr_free(&swr);
        m_swr = nullptr;
        av_frame_unref(f);
        return nextAudio(out);
    }

    int maxOut = (int)av_rescale_rnd(swr_get_delay(swr, f->sample_rate) + f->nb_samples,
                                     RATE, f->sample_rate, AV_ROUND_UP);
    out->pcm.resize((size_t)maxOut * CHANS);
    uint8_t *dst = (uint8_t *)out->pcm.data();

    int got = swr_convert(swr, &dst, maxOut,
                          (const uint8_t **)f->extended_data, f->nb_samples);
    double t = frame_time(m_a, f);
    av_frame_unref(f);

    if (got < 0) return false;
    out->frames = got;
    out->pcm.resize((size_t)got * CHANS);
    out->pts = t;
    return true;
}

bool Source::audioAt(double t, int frames, float *dst)
{
    const size_t need = (size_t)frames * CHANS;
    std::memset(dst, 0, need * sizeof(float));
    if (!hasAudio() || frames <= 0) return false;
    if (t < 0) t = 0;

    /* Continue where the last call left off unless the caller jumped. The
     * tolerance is one block at 48 kHz either way; anything further is a
     * scrub or a cut and deserves a real seek. */
    if (m_awant < 0.0 || t < m_awant - 0.03 || t > m_awant + 0.03)
        seekAudio(t);
    else
        t = m_awant;

    const double blockDur = frames / (double)RATE;

    /* Decode until the fifo covers [t, t + blockDur). */
    AudioBlock blk;
    int guard = 0;
    while (guard++ < 4096) {
        double have = m_afifoPts < 0 ? -1.0
                                     : m_afifoPts + (m_afifo.size() / CHANS) / (double)RATE;
        if (m_afifoPts >= 0 && have >= t + blockDur - 1e-9 && m_afifoPts <= t + 1e-9) break;

        if (!nextAudio(&blk)) break;   /* end of the file */

        if (m_afifo.empty()) {
            m_afifoPts = blk.pts >= 0 ? blk.pts : (m_afifoPts >= 0 ? m_afifoPts : t);
        }
        m_afifo.insert(m_afifo.end(), blk.pcm.begin(), blk.pcm.end());

        /* A seek lands on a packet boundary before t; drop what is behind. */
        if (m_afifoPts + 1e-9 < t) {
            long skip = lround((t - m_afifoPts) * RATE);
            size_t s = (size_t)std::max(0L, skip) * CHANS;
            if (s >= m_afifo.size()) {
                m_afifo.clear();
                m_afifoPts += (double)(skip) / RATE;
            } else if (s > 0) {
                m_afifo.erase(m_afifo.begin(), m_afifo.begin() + s);
                m_afifoPts += (double)(s / CHANS) / RATE;
            }
        }
    }

    size_t avail = m_afifo.size();
    size_t take = std::min(avail, need);
    if (take) std::memcpy(dst, m_afifo.data(), take * sizeof(float));

    if (take) m_afifo.erase(m_afifo.begin(), m_afifo.begin() + take);
    m_afifoPts = (m_afifoPts < 0 ? t : m_afifoPts) + (double)(take / CHANS) / RATE;
    m_awant = t + blockDur;

    return take == need;
}

/* ------------------------------------------------------------------ *
 * thumbnail
 * ------------------------------------------------------------------ */

bool thumbnail(const std::string &path, int w, int h, VideoFrame *out)
{
    std::string err;
    Source *s = Source::open(path, &err);
    if (!s) return false;
    if (!s->hasVideo()) { delete s; return false; }

    /* A tenth of the way in, which is past the fade-from-black on anything
     * edited and still inside anything short. */
    double t = s->info().duration > 1.0 ? s->info().duration * 0.1 : 0.0;

    bool ok = s->frameAt(t, out, w, h);
    if (!ok && t > 0) ok = s->frameAt(0, out, w, h);
    delete s;
    return ok;
}

} /* namespace sn */
