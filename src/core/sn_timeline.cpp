/*
 * BENCsnip - the edit
 */

#include "sn_timeline.h"

#include <algorithm>
#include <cmath>

namespace sn {

/* Times are seconds in a double. At an hour, a double still resolves about a
 * nanosecond, so the only thing that needs care is comparing two times that
 * ought to be equal after arithmetic - hence one epsilon, used everywhere. */
static const double EPS = 1e-6;

static bool before(double a, double b) { return a < b - EPS; }

/* ------------------------------------------------------------------ *
 * Track / Project lookups
 * ------------------------------------------------------------------ */

const Clip *Track::at(double t) const
{
    for (const Clip &c : clips)
        if (c.covers(t)) return &c;
    return nullptr;
}

Clip *Track::at(double t)
{
    return const_cast<Clip *>(static_cast<const Track *>(this)->at(t));
}

const BinItem *Project::item(int id) const
{
    for (const BinItem &b : bin)
        if (b.id == id) return &b;
    return nullptr;
}

BinItem *Project::item(int id)
{
    return const_cast<BinItem *>(static_cast<const Project *>(this)->item(id));
}

const Track *Project::track(int idx) const
{
    if (idx < 0 || idx >= (int)tracks.size()) return nullptr;
    return &tracks[idx];
}

Track *Project::track(int idx)
{
    return const_cast<Track *>(static_cast<const Project *>(this)->track(idx));
}

const Clip *Project::clip(const ClipRef &r) const
{
    const Track *t = track(r.track);
    if (!t) return nullptr;
    for (const Clip &c : t->clips)
        if (c.id == r.clip) return &c;
    return nullptr;
}

Clip *Project::clip(const ClipRef &r)
{
    return const_cast<Clip *>(static_cast<const Project *>(this)->clip(r));
}

double Project::duration() const
{
    double d = 0;
    for (const Track &t : tracks)
        if (!t.clips.empty()) d = std::max(d, t.clips.back().end());
    return d;
}

int Project::firstTrack(TrackKind k) const
{
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i].kind == k) return (int)i;
    return -1;
}

static void sort_track(Track &t)
{
    std::sort(t.clips.begin(), t.clips.end(),
              [](const Clip &a, const Clip &b) { return a.pos < b.pos; });
}

static bool track_free(const Track &t, double a, double b, int ignoreId = 0)
{
    for (const Clip &c : t.clips) {
        if (c.id == ignoreId) continue;
        if (before(c.pos, b) && before(a, c.end())) return false;
    }
    return true;
}

int Project::freeTrack(TrackKind k, double a, double b)
{
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i].kind == k && !tracks[i].locked && track_free(tracks[i], a, b))
            return (int)i;

    /* None free: add one, keeping video above audio in display order. */
    Track t;
    t.id = newId();
    t.kind = k;

    int n = 0;
    for (const Track &x : tracks) if (x.kind == k) n++;
    t.name = (k == TRACK_VIDEO ? "V" : "A") + std::to_string(n + 1);

    size_t insert = tracks.size();
    if (k == TRACK_VIDEO) {
        /* Video tracks stack upward: a new one goes on top. */
        insert = 0;
    }
    tracks.insert(tracks.begin() + insert, t);
    return (int)insert;
}

Project newProject()
{
    Project p;
    Track v;
    v.id = p.newId();
    v.kind = TRACK_VIDEO;
    v.name = "V1";
    Track a;
    a.id = p.newId();
    a.kind = TRACK_AUDIO;
    a.name = "A1";
    p.tracks.push_back(v);
    p.tracks.push_back(a);
    return p;
}

/* ------------------------------------------------------------------ *
 * Editing
 * ------------------------------------------------------------------ */

/* Make room over [a,b) on a track: anything overlapping is trimmed, split or
 * removed. A drop lands on top of what is there, which is what every editor
 * does and what dragging one clip onto another visibly means. */
static void clear_range(Track &t, double a, double b, int ignoreId)
{
    std::vector<Clip> out;
    out.reserve(t.clips.size() + 1);

    for (Clip c : t.clips) {
        if (c.id == ignoreId || !before(c.pos, b) || !before(a, c.end())) {
            out.push_back(c);
            continue;
        }

        const bool headKept = before(c.pos, a);
        const bool tailKept = before(b, c.end());

        if (headKept) {
            Clip h = c;
            h.out = h.in + (a - h.pos);
            h.fadeOut = std::min(h.fadeOut, h.dur());
            out.push_back(h);
        }
        if (tailKept) {
            Clip tl = c;
            tl.in = tl.in + (b - tl.pos);
            tl.pos = b;
            tl.fadeIn = std::min(tl.fadeIn, tl.dur());
            if (headKept) tl.id = 0;   /* caller renumbers - see addClip */
            out.push_back(tl);
        }
        /* Neither kept: the clip was entirely covered, so it is gone. */
    }

    t.clips.swap(out);
}

Clip *addClip(Project &p, int trackIdx, const Clip &in)
{
    Track *t = p.track(trackIdx);
    if (!t) return nullptr;

    Clip c = in;
    if (c.id == 0) c.id = p.newId();
    if (c.pos < 0) c.pos = 0;
    if (c.dur() <= EPS) return nullptr;

    clear_range(*t, c.pos, c.end(), c.id);
    for (Clip &x : t->clips)
        if (x.id == 0) x.id = p.newId();   /* the tail of something split */

    t->clips.push_back(c);
    sort_track(*t);
    p.dirty = true;

    for (Clip &x : t->clips)
        if (x.id == c.id) return &x;
    return nullptr;
}

double placeItem(Project &p, int itemId, double t, int videoTrack, int audioTrack)
{
    const BinItem *b = p.item(itemId);
    if (!b) return t;

    double dur = b->info.duration;
    if (dur <= 0) dur = 5.0;    /* a stream with no duration: give it five
                                   seconds, which the user can trim out */
    if (t < 0) t = 0;

    const int link = p.newId();
    const bool both = b->info.hasVideo && b->info.hasAudio &&
                      videoTrack != NO_TRACK && audioTrack != NO_TRACK;

    /* A negative track number below -1 means "not this kind at all", which is
     * what dropping a file onto an audio track asks for: the sound, and not a
     * video track invented above it. */
    if (b->info.hasVideo && videoTrack != NO_TRACK) {
        int tr = videoTrack >= 0 ? videoTrack : p.freeTrack(TRACK_VIDEO, t, t + dur);
        Clip c;
        c.source = itemId;
        c.in = 0;
        c.out = dur;
        c.pos = t;
        c.link = both ? link : 0;
        addClip(p, tr, c);
    }
    if (b->info.hasAudio && audioTrack != NO_TRACK) {
        int tr = audioTrack >= 0 ? audioTrack : p.freeTrack(TRACK_AUDIO, t, t + dur);
        Clip c;
        c.source = itemId;
        c.in = 0;
        c.out = dur;
        c.pos = t;
        c.link = both ? link : 0;
        addClip(p, tr, c);
    }

    p.dirty = true;
    return t + dur;
}

/* Every clip that moves with this one, as (track index, clip id). */
static std::vector<ClipRef> linked(const Project &p, const ClipRef &r)
{
    std::vector<ClipRef> v;
    const Clip *c = p.clip(r);
    if (!c) return v;
    v.push_back(r);
    if (!c->link) return v;

    for (size_t ti = 0; ti < p.tracks.size(); ti++)
        for (const Clip &x : p.tracks[ti].clips)
            if (x.link == c->link && !(x.id == c->id && (int)ti == r.track))
                v.push_back(ClipRef{(int)ti, x.id});
    return v;
}

bool removeClip(Project &p, const ClipRef &r, bool rippleAfter)
{
    const Clip *c = p.clip(r);
    if (!c) return false;

    const double a = c->pos, b = c->end();
    std::vector<ClipRef> group = linked(p, r);

    for (const ClipRef &g : group) {
        Track *t = p.track(g.track);
        if (!t) continue;
        for (size_t i = 0; i < t->clips.size(); i++)
            if (t->clips[i].id == g.clip) { t->clips.erase(t->clips.begin() + i); break; }
    }

    if (rippleAfter) ripple(p, b, -(b - a));
    p.dirty = true;
    return true;
}

double moveClip(Project &p, ClipRef &r, int newTrack, double newPos)
{
    Clip *c = p.clip(r);
    if (!c) return 0;

    const Track *dst = p.track(newTrack);
    if (!dst || dst->locked) newTrack = r.track;
    dst = p.track(newTrack);
    if (!dst) return c->pos;

    /* A clip only goes on a track of its own kind: video on video, audio on
     * audio. Dragging a video clip down into the audient half is a slip of
     * the mouse, not a request. */
    const Track *src = p.track(r.track);
    if (!src || dst->kind != src->kind) newTrack = r.track;

    if (newPos < 0) newPos = 0;

    const double delta = newPos - c->pos;
    const int dtrack = newTrack - r.track;

    /* Linked clips move together, and either all of them move or none do.
     * Each lifted clip is kept beside the reference it came from rather than
     * in a parallel vector: one clip that could not be found would otherwise
     * shift every later index by one and move the wrong clips. */
    struct Lifted {
        ClipRef from;
        Clip clip;
    };
    std::vector<Lifted> lifted;

    for (const ClipRef &g : linked(p, r)) {
        Track *t = p.track(g.track);
        if (!t) continue;
        for (size_t i = 0; i < t->clips.size(); i++)
            if (t->clips[i].id == g.clip) {
                lifted.push_back(Lifted{g, t->clips[i]});
                t->clips.erase(t->clips.begin() + i);
                break;
            }
    }

    /* Place them back at the new position, each on the corresponding track. */
    ClipRef moved = r;
    for (Lifted &l : lifted) {
        int tt = l.from.track + (l.from.track == r.track ? dtrack : 0);
        Track *t = p.track(tt);
        if (!t || t->kind != p.tracks[l.from.track].kind) {
            tt = l.from.track;
            t = p.track(tt);
        }
        if (!t) continue;

        l.clip.pos += delta;
        if (l.clip.pos < 0) l.clip.pos = 0;
        Clip *placed = addClip(p, tt, l.clip);
        if (l.from == r && placed) moved = ClipRef{tt, placed->id};
    }

    r = moved;
    p.dirty = true;
    const Clip *now = p.clip(r);
    return now ? now->pos : newPos;
}

void trimClip(Project &p, const ClipRef &r, bool head, double newEdge)
{
    Clip *c = p.clip(r);
    if (!c) return;

    const BinItem *b = p.item(c->source);
    const double srcDur = b && b->info.duration > 0 ? b->info.duration : 1e9;
    const double minDur = 0.02;

    std::vector<ClipRef> group = linked(p, r);

    for (const ClipRef &g : group) {
        Clip *x = p.clip(g);
        Track *t = p.track(g.track);
        if (!x || !t) continue;

        if (head) {
            double lim = 0;                   /* the clip before it        */
            for (const Clip &o : t->clips)
                if (o.id != x->id && o.end() <= x->pos + EPS) lim = std::max(lim, o.end());

            double e = newEdge;
            e = std::max(e, lim);
            e = std::max(e, x->pos - x->in);  /* cannot start before frame 0 */
            e = std::min(e, x->end() - minDur);

            const double d = e - x->pos;
            x->in += d;
            x->pos = e;
        } else {
            double lim = 1e18;                /* the clip after it         */
            for (const Clip &o : t->clips)
                if (o.id != x->id && o.pos >= x->end() - EPS) lim = std::min(lim, o.pos);

            double e = newEdge;
            e = std::min(e, lim);
            e = std::min(e, x->pos + (srcDur - x->in));
            e = std::max(e, x->pos + minDur);

            x->out = x->in + (e - x->pos);
        }

        x->fadeIn = std::min(x->fadeIn, x->dur());
        x->fadeOut = std::min(x->fadeOut, x->dur());
        sort_track(*t);
    }
    p.dirty = true;
}

int splitAt(Project &p, double t, const ClipRef *only)
{
    int n = 0;

    for (size_t ti = 0; ti < p.tracks.size(); ti++) {
        Track &tr = p.tracks[ti];
        if (tr.locked) continue;
        if (only && only->track != (int)ti) continue;

        std::vector<Clip> add;
        for (Clip &c : tr.clips) {
            if (only && c.id != only->clip) continue;
            if (!before(c.pos, t) || !before(t, c.end())) continue;

            Clip tail = c;
            tail.id = p.newId();
            tail.in = c.srcAt(t);
            tail.pos = t;
            tail.fadeIn = 0;
            /* A split breaks the link: the two halves are separate clips
             * now, and dragging one should not drag the other. The video and
             * audio halves of the same split keep their own pairing, which is
             * what the new link id below is for. */
            tail.link = c.link ? p.newId() : 0;

            c.out = tail.in;
            c.fadeOut = 0;

            add.push_back(tail);
            n++;
        }

        /* Give both halves of a linked cut the same new link id. */
        int newLink = 0;
        for (Clip &x : add) {
            if (x.link) {
                if (!newLink) newLink = x.link;
                x.link = newLink;
            }
        }
        for (Clip &x : add) tr.clips.push_back(x);
        sort_track(tr);
    }

    if (n) p.dirty = true;
    return n;
}

void ripple(Project &p, double t, double delta, int exceptTrack)
{
    for (size_t ti = 0; ti < p.tracks.size(); ti++) {
        if ((int)ti == exceptTrack) continue;
        Track &tr = p.tracks[ti];
        if (tr.locked) continue;
        for (Clip &c : tr.clips)
            if (c.pos >= t - EPS) c.pos = std::max(0.0, c.pos + delta);
        sort_track(tr);
    }
    p.dirty = true;
}

int closeGap(Project &p, double t, int trackIdx)
{
    /* The gap is from the end of whatever precedes t to the start of whatever
     * follows it, taken across every track at once so the tracks stay in
     * sync. Closing a gap on one track alone is what the trackIdx argument is
     * for, and it is not what the keyboard shortcut does. */
    double gapStart = 0, gapEnd = 1e18;
    bool any = false;

    for (size_t ti = 0; ti < p.tracks.size(); ti++) {
        if (trackIdx >= 0 && (int)ti != trackIdx) continue;
        const Track &tr = p.tracks[ti];
        for (const Clip &c : tr.clips) {
            if (c.covers(t)) return 0;              /* not in a gap        */
            if (c.end() <= t + EPS) gapStart = std::max(gapStart, c.end());
            if (c.pos >= t - EPS) { gapEnd = std::min(gapEnd, c.pos); any = true; }
        }
    }

    if (!any || gapEnd <= gapStart + EPS) return 0;
    ripple(p, gapEnd, -(gapEnd - gapStart));
    return 1;
}

double snap(const Project &p, double t, double tol, double playhead,
            const ClipRef *ignore)
{
    double best = t, bestD = tol;

    auto tryPoint = [&](double x) {
        double d = std::fabs(x - t);
        if (d < bestD) { bestD = d; best = x; }
    };

    tryPoint(0.0);
    if (playhead >= 0) tryPoint(playhead);

    for (size_t ti = 0; ti < p.tracks.size(); ti++)
        for (const Clip &c : p.tracks[ti].clips) {
            if (ignore && ignore->track == (int)ti && ignore->clip == c.id) continue;
            tryPoint(c.pos);
            tryPoint(c.end());
        }

    return best;
}

double prevEdit(const Project &p, double t)
{
    double best = 0;
    for (const Track &tr : p.tracks)
        for (const Clip &c : tr.clips) {
            if (c.pos < t - EPS) best = std::max(best, c.pos);
            if (c.end() < t - EPS) best = std::max(best, c.end());
        }
    return best;
}

double nextEdit(const Project &p, double t)
{
    double best = p.duration();
    bool any = false;
    for (const Track &tr : p.tracks)
        for (const Clip &c : tr.clips) {
            if (c.pos > t + EPS) { best = std::min(best, c.pos); any = true; }
            if (c.end() > t + EPS) { best = std::min(best, c.end()); any = true; }
        }
    return any ? best : t;
}

/* ------------------------------------------------------------------ *
 * History
 * ------------------------------------------------------------------ */

void History::reset(const Project &p)
{
    m_states.clear();
    m_states.push_back(p);
    m_at = 0;
}

void History::commit(const Project &p)
{
    if (m_at < 0) { reset(p); return; }

    /* Anything redone-past is gone the moment a new edit happens, which is
     * the behaviour every program has and the only one nobody has to think
     * about. */
    m_states.resize(m_at + 1);
    m_states.push_back(p);
    m_at = (int)m_states.size() - 1;

    if ((int)m_states.size() > LIMIT) {
        m_states.erase(m_states.begin());
        m_at--;
    }
}

bool History::undo(Project *p)
{
    if (!canUndo()) return false;
    m_at--;
    *p = m_states[m_at];
    p->dirty = true;
    return true;
}

bool History::redo(Project *p)
{
    if (!canRedo()) return false;
    m_at++;
    *p = m_states[m_at];
    p->dirty = true;
    return true;
}

} /* namespace sn */
