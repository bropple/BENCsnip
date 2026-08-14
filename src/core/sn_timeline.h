/*
 * BENCsnip - the edit
 *
 * A project is a bin of files and a timeline of tracks of clips. A clip is a
 * range of one file placed at a time, and that is the whole data model - there
 * is no transition object, no nested sequence, no effect graph. Trimming a
 * clip moves `in` or `out`; sliding it moves `pos`; splitting it makes two
 * clips out of one. Nothing here decodes anything.
 *
 * Editing operations are free functions rather than methods because they all
 * do the same three things - snapshot for undo, change the vector, re-sort -
 * and a method that forgets the first one is a bug you only find later.
 */

#ifndef SN_TIMELINE_H
#define SN_TIMELINE_H

#include "sn_media.h"

#include <string>
#include <vector>

namespace sn {

/* A file the project knows about. The bin owns these; clips refer to one by
 * id, so relinking a moved file is one edit in one place. */
struct BinItem {
    int id = 0;
    MediaInfo info;
    bool missing = false;    /* the file was there when it was imported */
};

struct Clip {
    int id = 0;
    int source = 0;          /* BinItem::id                              */
    int link = 0;            /* clips sharing a nonzero link move as one */

    double in = 0.0;         /* source seconds, inclusive                */
    double out = 0.0;        /* source seconds, exclusive                */
    double pos = 0.0;        /* timeline seconds of `in`                 */

    double gain = 1.0;       /* audio, linear                            */
    double fadeIn = 0.0;     /* seconds                                  */
    double fadeOut = 0.0;
    bool muted = false;

    double dur() const { return out - in; }
    double end() const { return pos + dur(); }
    /* Source time under a timeline time inside this clip. */
    double srcAt(double t) const { return in + (t - pos); }
    bool covers(double t) const { return t >= pos && t < end(); }
};

enum TrackKind { TRACK_VIDEO = 0, TRACK_AUDIO = 1 };

struct Track {
    int id = 0;
    TrackKind kind = TRACK_VIDEO;
    std::string name;
    bool muted = false;      /* audio silent / video hidden              */
    bool locked = false;     /* the mouse leaves it alone                */
    int height = 0;          /* 0 = the view's default                   */
    std::vector<Clip> clips; /* kept sorted by pos, always               */

    const Clip *at(double t) const;
    Clip *at(double t);
};

/* Where a clip lives, for the GUI's selection. Either half being negative
 * means "nothing". */
struct ClipRef {
    int track = -1;
    int clip = -1;           /* Clip::id, not an index - indices move    */
    bool ok() const { return track >= 0 && clip > 0; }
    bool operator==(const ClipRef &o) const { return track == o.track && clip == o.clip; }
};

struct Project {
    std::string path;              /* where it was saved, "" if never    */
    std::string name = "untitled";

    std::vector<BinItem> bin;
    std::vector<Track> tracks;     /* display order, top to bottom;
                                      video tracks first, then audio     */

    /* What the preview and a default export use. Set from the first video
     * dropped in, then left alone unless the user changes it. */
    int width = 1920, height = 1080;
    double fps = 30.0;

    bool dirty = false;
    int nextId = 1;

    /* --- lookups --- */
    const BinItem *item(int id) const;
    BinItem *item(int id);
    const Track *track(int idx) const;
    Track *track(int idx);
    Clip *clip(const ClipRef &r);
    const Clip *clip(const ClipRef &r) const;

    double duration() const;             /* end of the last clip         */
    int newId() { return nextId++; }

    /* Index of the first video/audio track, or -1. */
    int firstTrack(TrackKind k) const;
    /* A track of that kind that is free over [a,b), adding one if none is. */
    int freeTrack(TrackKind k, double a, double b);
};

/* A project with one video track and one audio track, which is what an empty
 * window should show: dropping a file has somewhere to land. */
Project newProject();

/* --- editing ------------------------------------------------------- *
 * All of these keep each track's clips sorted and non-overlapping. None of
 * them touch the undo stack; the caller does that, because only the caller
 * knows whether a drag is one edit or sixty frames of one.
 * ------------------------------------------------------------------- */

/* Put a clip on a track, pushing nothing aside: whatever it lands on top of
 * is trimmed or split around it. This is what a drop does. */
Clip *addClip(Project &p, int trackIdx, const Clip &c);

/* Import a bin item at time t. Video and audio go on separate tracks and are
 * linked. -1 for a track means "find or make one"; NO_TRACK means "none of
 * that kind", which is what dropping a file onto an audio track asks for.
 * Returns the timeline time just past what it added. */
enum { NO_TRACK = -2 };
double placeItem(Project &p, int itemId, double t, int videoTrack = -1,
                 int audioTrack = -1);

bool removeClip(Project &p, const ClipRef &r, bool ripple = false);

/* Move a clip to a new position and possibly a new track. Returns where it
 * actually landed, which may differ if it was blocked. */
double moveClip(Project &p, ClipRef &r, int newTrack, double newPos);

/* Drag an edge. `head` trims `in`/`pos` together, keeping the source content
 * under the mouse still; the tail trims `out`. Both stop at the source's own
 * limits and at the neighbouring clip. */
void trimClip(Project &p, const ClipRef &r, bool head, double newEdge);

/* Cut every unlocked track at t, or one clip if `only` is set. Returns how
 * many cuts were made. */
int splitAt(Project &p, double t, const ClipRef *only = nullptr);

/* Close the gap that starts at t on every track (or one), sliding everything
 * after it back. */
int closeGap(Project &p, double t, int trackIdx = -1);

/* Everything at or after t moves by delta on every track. */
void ripple(Project &p, double t, double delta, int exceptTrack = -1);

/* The nearest edit point to t within `tol` seconds: clip edges, the playhead
 * and zero. Returns t unchanged when nothing is near. */
double snap(const Project &p, double t, double tol, double playhead,
            const ClipRef *ignore = nullptr);

/* Clip edges on either side of t across all tracks, for the ,/. keys. */
double prevEdit(const Project &p, double t);
double nextEdit(const Project &p, double t);

/* --- undo ---------------------------------------------------------- *
 * Whole-project snapshots. A timeline is a few kilobytes of vectors even for
 * a long edit, so the clever thing - a command stack with an inverse for each
 * operation - would cost more in bugs than it saves in memory.
 * ------------------------------------------------------------------- */
class History {
public:
    void reset(const Project &p);

    /* Call AFTER an edit, not before: the top of the stack is always the
     * project as it is now. The other arrangement - recording the state
     * before each change - needs the current state pushed as well the first
     * time anyone undoes, and gets that wrong in exactly the case nobody
     * tests, which is undo followed by redo. */
    void commit(const Project &p);

    bool undo(Project *p);
    bool redo(Project *p);
    bool canUndo() const { return m_at > 0; }
    bool canRedo() const { return m_at + 1 < (int)m_states.size(); }

private:
    std::vector<Project> m_states;
    int m_at = -1;
    enum { LIMIT = 200 };
};

} /* namespace sn */

#endif /* SN_TIMELINE_H */
