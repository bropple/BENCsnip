/*
 * BENCsnip - writing it out
 *
 * Two paths, and the difference matters enough to be visible in the UI:
 *
 *   Fast trim   The timeline is one clip of one file with nothing done to it
 *               but a trim. Packets are copied straight across, so a 4 GB
 *               camera file takes a second and loses nothing, at the cost of
 *               starting at the keyframe before the in point.
 *
 *   Render      Everything else. Every output frame is composited and encoded.
 *
 * exportTimeline picks the first when the settings allow it and it applies.
 */

#ifndef SN_EXPORT_H
#define SN_EXPORT_H

#include "sn_timeline.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace sn {

struct ExportSettings {
    std::string path;

    int width = 1920, height = 1080;
    double fps = 30.0;

    /* libav encoder names. Empty means "no stream of this kind". */
    std::string vcodec = "libx264";
    std::string acodec = "aac";

    int crf = 20;             /* quality for x264/x265/vpx; ignored by others */
    int64_t vbitrate = 0;     /* bits/s; 0 means use crf                      */
    int64_t abitrate = 192000;
    std::string preset = "medium";

    double from = 0.0;
    double to = -1.0;         /* < 0 means the end of the timeline            */

    bool allowCopy = true;    /* try the fast path when it applies            */
};

/* Written by the export thread, read by the GUI. */
struct ExportStatus {
    std::atomic<double> progress{0.0};   /* 0..1                             */
    std::atomic<bool> cancel{false};
    std::atomic<bool> running{false};
    std::atomic<bool> ok{false};
    std::atomic<bool> copied{false};     /* the fast path was taken          */

    std::mutex lock;
    std::string message;                 /* stage, or the error              */

    void say(const std::string &s) { std::lock_guard<std::mutex> g(lock); message = s; }
    std::string said() { std::lock_guard<std::mutex> g(lock); return message; }
};

/* Blocking. Runs on whatever thread calls it - the GUI hands it a std::thread
 * and watches the status. The project must not be edited while it runs, so
 * the caller passes a copy. */
bool exportTimeline(const Project &p, const ExportSettings &s, ExportStatus *st);

/* Whether the fast path applies, and if not, why not - the export dialog says
 * so rather than silently doing the slow thing. */
bool canStreamCopy(const Project &p, const ExportSettings &s, std::string *why);

/* Encoders this build of ffmpeg actually has, best first, for the format
 * chooser. Returns names usable as ExportSettings::vcodec. */
std::vector<std::string> videoEncoders();
std::vector<std::string> audioEncoders();

/* The container libav would infer from a filename, e.g. "mp4", or "" if it
 * cannot tell. */
std::string containerFor(const std::string &path);

} /* namespace sn */

#endif /* SN_EXPORT_H */
