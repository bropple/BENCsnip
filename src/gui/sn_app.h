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
    DRAG_FADE_IN,     /* the fade handle at the top left        */
    DRAG_FADE_OUT,
    DRAG_GAIN,        /* the level line across an audio clip    */
    DRAG_SCRUB,       /* the playhead, from the ruler           */
    DRAG_FROM_BIN,    /* a bin item on its way to the timeline  */
    DRAG_LAYER,       /* a track's picture, moved on the preview */
    DRAG_LAYER_SIZE   /* ...and resized by one of its handles    */
};

enum Modal {
    MODAL_NONE = 0,
    MODAL_EXPORT,
    MODAL_LAYOUT,     /* one track's size, position and crop        */
    MODAL_CANVAS,     /* the project's own size and frame rate      */
    MODAL_INFO,
    MODAL_OPEN,       /* the file browser, importing            */
    MODAL_SAVE,
    MODAL_LOAD,
    MODAL_CONFIRM
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

    /* --- the preview --- */
    Texture2D preview = {};
    int previewW = 0, previewH = 0;

    /* --- the bin --- */
    std::map<int, Thumb> thumbs;
    float binScroll = 0.0f;

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
void binShutdown(App &a);
void timelinePane(App &a, Rectangle r);
void previewPane(App &a, Rectangle r);

/* --- the mascot --- */
enum TarrMood { TARR_IDLE, TARR_BUSY, TARR_HAPPY, TARR_SORRY };
void tarr(App &a, Vector2 center, float radius, TarrMood mood);
/* The line S. Tarr says on an empty timeline. Deadpan, and it does not
 * change every frame. */
const char *tarrLine(int which);

/* --- modals --- */
void exportDialog(App &a);
void layoutDialog(App &a);          /* one track's size, position and crop */
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
