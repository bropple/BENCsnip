/*
 * BENCsnip - what the window is looking at
 *
 * One struct, passed to every pane. An editor is a small program with a lot of
 * shared state - the project, the selection, where the playhead is, how far
 * the timeline is zoomed - and threading that through as arguments produces
 * either twelve-parameter functions or a set of globals pretending not to be
 * one.
 *
 * Nothing here decodes or draws; the panes do that.
 */

#ifndef SN_APP_H
#define SN_APP_H

#include "sn_export.h"
#include "sn_gui.h"
#include "sn_peaks.h"
#include "sn_player.h"
#include "sn_project.h"
#include "sn_timeline.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sn {

/* What the mouse is in the middle of. A drag has to survive the pointer
 * leaving the thing it started on, so the state lives here rather than in the
 * pane that started it. */
enum DragKind {
    DRAG_NONE = 0,
    DRAG_CLIP,        /* moving a clip                          */
    DRAG_TRIM_IN,     /* its left edge                          */
    DRAG_TRIM_OUT,    /* its right edge                         */
    DRAG_FX,          /* a point on the effects lane, moved     */
    DRAG_FX_SWEEP,    /* ...or a stretch of it being marked out */
    DRAG_GAIN,        /* the level line across an audio clip    */
    DRAG_SCRUB,       /* the playhead, from the ruler           */
    DRAG_FROM_BIN,    /* a bin item on its way to the timeline  */
    DRAG_LAYER,       /* a track's picture, moved on the preview */
    DRAG_LAYER_SIZE,  /* ...and resized by one of its handles    */
    DRAG_TEXT,        /* a caption, moved on the preview         */
    DRAG_TEXT_SIZE,   /* ...resized by a corner                  */
    DRAG_TEXT_ROT     /* ...turned by the handle above it        */
};

enum Modal {
    MODAL_NONE = 0,
    MODAL_EXPORT,
    MODAL_CANVAS,     /* the project's own size and frame rate      */
    MODAL_INFO,
    MODAL_HELP,       /* every control there is                     */
    MODAL_OPEN,       /* the file browser, importing            */
    MODAL_SAVE,
    MODAL_LOAD,
    MODAL_CONFIRM
};

/* A panel that slides in from the edge of the window.
 *
 * The media bin and the inspector are the same thing on opposite sides: a tab
 * at the edge, a column that comes out when it is asked for, and a pin for
 * when somebody wants it to stay. Unpinned it closes itself a few seconds
 * after the pointer leaves, because a panel that is only wanted for one
 * adjustment should not have to be dismissed afterwards.
 *
 * The column takes its width from the middle of the window rather than
 * covering it. A panel that overlays the preview hides the thing being
 * adjusted, which for the inspector is exactly the wrong half to hide.
 */
struct Drawer {
    bool open = false;
    bool pinned = false;

    /* How wide it is drawn right now, sliding towards where it should be.
     * Animated because a column that appears between one frame and the next
     * reads as the window having jumped rather than as a panel having
     * opened. */
    float w = 0;

    /* When the pointer was last inside it, for the timeout. */
    double touched = 0;
};

struct Thumb {
    Texture2D tex = {};
    bool ready = false;
    bool asked = false;
};

struct App {
    sn_ui ui;

    Project proj;
    History hist;
    uint64_t rev = 1;          /* bumped whenever the project changes    */

    Player player;

    /* --- what is selected --- */
    std::vector<ClipRef> sel;
    int selBin = 0;            /* BinItem::id, or 0                      */

    /* --- the timeline view --- */
    double zoom = 60.0;        /* pixels per second                      */
    double scroll = 0.0;       /* seconds at the left edge               */
    float trackScroll = 0.0f;  /* pixels the track list is scrolled down */
    double playhead = 0.0;
    bool snapping = true;
    bool follow = true;        /* scroll to keep the playhead in view    */

    /* --- the drag in progress --- */
    DragKind drag = DRAG_NONE;
    ClipRef dragClip;
    int dragBin = 0;
    double dragGrab = 0.0;     /* seconds between the pointer and the
                                  clip's start, so it does not jump      */
    Vector2 dragFrom = {0, 0};
    bool dragMoved = false;

    /* What each row of the clip menu does.
     *
     * The menu's rows are not fixed: splitting channels only appears on an
     * audio clip of a file that has more than one, and unlinking only on a
     * clip that is linked. It used to be one optional row, placed last so the
     * handler could find it by counting; with two, counting is a bug waiting
     * for whoever adds the third. The pane fills this in as it builds the
     * menu and the handler reads the action out of it. */
    enum ClipAction {
        CLIP_SPLIT = 0,
        CLIP_DELETE,
        CLIP_RIPPLE,
        CLIP_MUTE,
        CLIP_CLEAR_FX,
        CLIP_SPLIT_CHANNELS,
        CLIP_UNLINK,
        CLIP_NOTHING
    };
    std::vector<int> clipMenu;

    /* What each row of the effects lane's menu does, for the same reason
     * clipMenu exists: the rows depend on whether there is a clip under the
     * pointer and whether a point was right-clicked. */
    enum FxAction {
        FX_M_IN = 0,
        FX_M_OUT,
        FX_M_INOUT,
        FX_M_PULSE,
        FX_M_WAVE,
        FX_M_HOLD,
        FX_M_DELETE,
        FX_M_CLEAR,
        FX_M_NOTHING
    };
    std::vector<int> fxMenu;
    double fxRangeFrom = 0, fxRangeTo = 0;   /* what a preset would cover */

    /* A stretch of one lane marked out with Shift and the mouse, for a preset
     * to be applied over. -1 for the track when there is none.
     *
     * It exists because the other three ways of saying where a preset goes -
     * the selection, the clip under the pointer, the whole track - are all
     * somebody else's idea of a range, and sometimes the range wanted is not
     * any of them. */
    int fxSweepTrack = -1;
    double fxSweepFrom = 0, fxSweepTo = 0;
    bool fxSweepOk() const { return fxSweepTrack >= 0 && fxSweepTo > fxSweepFrom; }

    /* Which point on which effects lane is selected, and which is being
     * dragged. An index rather than an id: effects are a small sorted vector
     * on a track and nothing outside this refers to one. */
    struct FxRef {
        int track = -1;
        int index = -1;
        bool ok() const { return track >= 0 && index >= 0; }
    };
    FxRef fxSel;
    FxRef fxDrag;
    double fxGrabFrom = 0, fxGrabTo = 0;   /* the ramp when the drag began */
    double fxNewAt = 0;                    /* where a new one was started  */

    /* Which track's level fader is being dragged, or -1. It is not one of the
     * drags above because the fader is a widget and does its own dragging;
     * this is here only so the undo history gets one entry for the adjustment
     * rather than one for every pixel of it. */
    int gainTrack = -1;

    /* --- the preview --- */
    Texture2D preview = {};
    int previewW = 0, previewH = 0;

    /* --- the bin --- */
    std::map<int, Thumb> thumbs;
    float binScroll = 0.0f;

    /* --- the two drawers --- */
    /* The bin starts open and pinned, which is where it has always been.
     * A window that comes up with no media pane is a window that looks like
     * it lost one; somebody who wants the room can unpin it and it will get
     * out of the way by itself from then on. */
    Drawer bin;             /* the media pane, on the left  */
    Drawer inspect;         /* what is selected, on the right */

    /* What the inspector is showing: a track's layout, or a caption. Set by
     * whatever asked for it to open. */
    enum InspectPage { INSPECT_LAYOUT = 0, INSPECT_TEXT };
    int inspectPage = INSPECT_LAYOUT;
    float inspectScroll = 0.0f;

    /* --- modals --- */
    Modal modal = MODAL_NONE;
    std::string confirmText;
    int confirmTag = 0;

    /* --- export --- */
    ExportSettings ex;
    ExportStatus exStatus;
    std::unique_ptr<std::thread> exThread;

    /* Which track the layout window is about, and which one is selected on
     * the preview - the same number, so opening the window from a selection
     * needs no extra state. */
    int layoutTrack = -1;

    /* --- captions ---
     *
     * Which one the text window is about. Not a second selection: it is set
     * from `sel` whenever a caption is picked, on the timeline or on the
     * canvas, so there is one idea of what is selected and the two panes
     * cannot disagree about it.
     *
     * `textGrab` is the style as it was when a drag started, so that moving,
     * resizing and turning all work from where the caption was rather than
     * from where it got to on the previous frame - which is how a drag
     * accumulates rounding until the thing being dragged drifts. */
    ClipRef textClip;
    TextStyle textGrab;
    int textHandle = -1;      /* 0..3 clockwise from the top left     */
    double textRotGrab = 0.0; /* the pointer's angle when it started  */

    /* Which handle of the selected layer is being dragged: 0..7 clockwise
     * from the top left, -1 when the body is being moved. */
    int layerHandle = -1;
    Rectangle layerGrab = {0, 0, 0, 0};   /* the rect when the drag began */
    Vector2 layerFrom = {0, 0};

    /* --- the title in the toolbar --- */
    bool renaming = false;
    std::string renameText;

    /* --- the status line --- */
    std::string status;
    double statusAt = 0.0;
    bool statusBad = false;

    /* --- layout, filled in every frame by main --- */
    Rectangle rBin = {0, 0, 0, 0};
    Rectangle rPreview = {0, 0, 0, 0};
    Rectangle rTimeline = {0, 0, 0, 0};
    Rectangle rStatus = {0, 0, 0, 0};

    bool quit = false;

    /* --- helpers --- */
    void say(const char *fmt, ...);
    void complain(const char *fmt, ...);

    /* Call after any change to `proj`. Bumps the revision the player watches
     * and, unless `minor`, pushes an undo state. */
    void changed(bool minor = false);

    double timeAt(float x) const;      /* screen x -> timeline seconds   */
    float xAt(double t) const;         /* and back                       */

    bool selected(const ClipRef &r) const;
    void select(const ClipRef &r, bool add);
    void clearSel();
};

/* --- the panes --- */
void binPane(App &a, Rectangle r);
void inspectPane(App &a, Rectangle r);

/* One drawer's tab, at the edge of the window. Returns true when it was
 * clicked. `left` says which edge it hangs on. */
bool drawerTab(App &a, Drawer &d, Rectangle tab, sn_icon icon, const char *name,
               bool left);

/* Slide it towards where it should be, and close it when it has been left
 * alone for long enough. `full` is how wide it is when open. */
void drawerStep(App &a, Drawer &d, Rectangle r, float full, bool busy);
void binShutdown(App &a);
void timelinePane(App &a, Rectangle r);
/* How long each part of starting up took, for the info window. Filled in by
 * main as it goes; see the note beside the marks there. */
int startupPhases();
const char *startupPhaseName(int i);
double startupPhaseMs(int i);
double startupWindowMs();
double startupReadyMs();
const char *glRenderer();
const char *glVendor();

void previewPane(App &a, Rectangle r);

/* --- the mascot --- */
enum TarrMood { TARR_IDLE, TARR_BUSY, TARR_HAPPY, TARR_SORRY };
void tarr(App &a, Vector2 center, float radius, TarrMood mood);
/* The line S. Tarr says on an empty timeline. Deadpan, and it does not
 * change every frame. */
const char *tarrLine(int which);

/* --- modals --- */
void exportDialog(App &a);
void helpDialog(App &a);            /* every control, in one table          */
void canvasDialog(App &a);          /* the project's own size and rate     */
void exportDialogPrepare(App &a);   /* call as the dialog opens */
/* The project was renamed: the suggested output filename follows it, unless
 * the person has already typed one of their own. */
void exportRenamed(App &a);
void infoWindow(App &a);
void confirmDialog(App &a);
void startExport(App &a);

/* --- the file browser --- */
/* Returns 1 when a path was chosen (in `out`), -1 when cancelled, 0 while it
 * is still up. `save` puts a name field on it. */
int fileDialog(App &a, Rectangle r, const char *title, bool save, std::string *out);
void fileDialogOpen(const std::string &startDir, const std::string &suggested);

/* --- shared bits of editing the panes both need --- */
void doSplit(App &a);
void doDelete(App &a, bool ripple);
void doImport(App &a, const std::string &path, bool place);
void zoomTo(App &a, double centreSeconds, double newZoom);
void zoomFit(App &a);

} /* namespace sn */

#endif /* SN_APP_H */
