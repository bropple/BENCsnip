/*
 * BENCsnip GUI - theme and widget set
 *
 * Drawn here rather than taken from a toolkit, for the same reason BENCsynth
 * and BENCmouth draw their own: the BENCO look is flat fills, thin dim borders
 * and small radii, which is what an immediate-mode renderer produces by
 * default, and a native widget set would have to be argued out of its
 * scrollbars, focus rings and animations at every step. A clip on a timeline
 * is not in any toolkit anyway.
 *
 * Every colour here comes from the BENCO style guide and nowhere else.
 */

#ifndef SN_GUI_H
#define SN_GUI_H

#include "raylib.h"

#include <string>

/* ------------------------------------------------------------------ *
 * Palette
 * ------------------------------------------------------------------ */

extern Color SN_BG;        /* window background, a green-tinted near-black */
extern Color SN_WELL;      /* the recess a panel sits in                   */
extern Color SN_PANEL;     /* a panel's face                               */
extern Color SN_PANEL_HI;  /* its header, and a hovered control            */
extern Color SN_BORDER;    /* 1px, dim, never decorative                   */
extern Color SN_TEXT;      /* phosphor - the screen-glow green, not white  */
extern Color SN_DIM;       /* labels, captions, anything secondary         */
extern Color SN_ACCENT;    /* the brand green: fills, active states        */
extern Color SN_EDGE;      /* pressed states and outlines                  */
extern Color SN_ALERT;     /* errors, the record dot, S. Tarr's stripe     */
extern Color SN_AMBER;     /* warnings, the playhead                       */
extern Color SN_STAR;      /* S. Tarr                                      */
extern Color SN_STAR_EDGE;
extern Color SN_VISOR;

/* Clips are coloured by what is in them, not by which file they came from:
 * one glance says "picture" or "sound" without reading anything. The two are
 * roster colours - P. Gon's blue for video, R. Triy's green for audio - kept
 * dark enough that white-ish text sits on them legibly. */
extern Color SN_TIP;
extern Color SN_CLIP_V, SN_CLIP_V_HI, SN_CLIP_V_EDGE;
extern Color SN_CLIP_A, SN_CLIP_A_HI, SN_CLIP_A_EDGE;
extern Color SN_CLIP_T, SN_CLIP_T_HI, SN_CLIP_T_EDGE;

#define SN_RADIUS 3
#define SN_PAD    8

/* Terminus is a bitmap design: crisp at its native sizes, mush between them.
 * These are native sizes and the font is loaded with point filtering to keep
 * it that way. */
enum {
    SN_F_TINY = 12,   /* clip labels, timecodes on the ruler */
    SN_F_SMALL = 16,  /* buttons, panel titles               */
    SN_F_BODY = 20,   /* the status line                     */
    SN_F_TITLE = 28   /* the wordmark                        */
};

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */

typedef struct sn_ui {
    Font tiny, small, body, title;
    int loaded;

    /* Which control has the mouse, by caller-chosen id. One at a time, so a
     * drag that wanders off its own rectangle keeps going - which is what
     * dragging a clip is, most of the time. Zero is nobody. */
    int active;
    float grabX, grabY;   /* pointer where the drag started */
    float grabV;          /* the control's value then       */

    /* Double-click detection. */
    int lastId;
    double lastClick;

    /* A menu drawn over the layout has to draw after everything it covers and
     * take the mouse away from what is underneath. Immediate mode gives
     * neither for free, so the menu publishes its rectangle here as it is
     * declared, widgets called later ignore a mouse inside it, and the
     * drawing is deferred to sn_ui_overlay. */
    int menuOpen;
    Rectangle menuRect;
    const char **menuItems;
    int menuCount;
    int menuHover;
    int menuTag;
    int menuFresh;        /* set on the frame it opens - see sn_menu_take */

    /* Which text field has the keyboard, by caller-chosen id. While it is
     * set, the window stops feeding keystrokes to the shortcuts - otherwise
     * typing a filename with an "s" in it also splits the clip. */
    int focus;
    int caret;

    /* The other end of a selection. Equal to caret when nothing is selected,
     * which is the state everything else tests for. */
    int anchor;

    /* Set while the pointer is sweeping out a selection, so the field keeps
     * following the mouse until it is let go - including outside the box. */
    int fieldDrag;

    /* When the caret last did anything. It stops blinking for a moment
     * afterwards: a caret that happens to be in its dark half while somebody
     * is arrowing along a path is a caret they have to stop and wait for. */
    double caretLive;

    /* Set by the caller around controls that must not take the mouse this
     * frame - anything under a modal, or everything at all while a clip is
     * being dragged across the timeline. */
    int suppress;

    /* One line of help, published by whatever the pointer is over and drawn
     * at the bottom of the window. Cleared every frame. */
    char tip[160];

    /* And the pointer itself. Panes ask for a shape; the window sets it once
     * at the end of the frame. Asking raylib directly from each pane means
     * whichever drew last wins, which is a cursor that flickers between two
     * shapes depending on paint order. */
    int cursor;

    /* A shape the operating system does not have. There is no "this will
     * loop" cursor in any standard set, so when one is asked for the system
     * pointer is hidden and the glyph is drawn in its place. -1 is nobody,
     * which is nearly always. */
    int cursorGlyph;
} sn_ui;

void sn_ui_init(sn_ui *ui);
void sn_ui_free(sn_ui *ui);
void sn_ui_frame(sn_ui *ui);       /* call once at the top of each frame */
void sn_ui_overlay(sn_ui *ui);     /* call last: menus draw here         */

int sn_ui_blocked(const sn_ui *ui);
int sn_double_click(sn_ui *ui, int id);
void sn_tip(sn_ui *ui, const char *fmt, ...);

/* What the pointer should look like here. The most specific request in a
 * frame wins: a resize beats a move beats a hand beats the arrow, because
 * they are asked for in that order by things that overlap. */
void sn_cursor(sn_ui *ui, int shape);


/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

void sn_text(const sn_ui *ui, int size, const char *s, float x, float y, Color c);
void sn_text_spaced(const sn_ui *ui, int size, const char *s, float x, float y, Color c);
void sn_text_center(const sn_ui *ui, int size, const char *s, float cx, float y, Color c);
float sn_measure(const sn_ui *ui, int size, const char *s, float spacing);
/* As much of `s` as fits in `w`, ellipsised with a leading "..." - the end of
 * a filename tells you more than the start of one. */
void sn_text_clip(const sn_ui *ui, int size, const char *s, float x, float y,
                  float w, Color c);

/* ------------------------------------------------------------------ *
 * Chrome
 * ------------------------------------------------------------------ */

void sn_panel(Rectangle r, Color fill, Color border);
void sn_divider(float x, float y, float w);

int sn_button(sn_ui *ui, int id, Rectangle r, const char *label, int enabled);
int sn_button_lit(sn_ui *ui, int id, Rectangle r, const char *label, int lit);

/* A control drawn as a glyph rather than a word: the transport, the tools,
 * the per-track switches. Everything in one enum so a caller never has to
 * know how any of them are drawn. */
typedef enum {
    SN_I_PLAY, SN_I_PAUSE, SN_I_STOP, SN_I_START, SN_I_END, SN_I_PREV, SN_I_NEXT,
    SN_I_SPLIT, SN_I_TRASH, SN_I_PLUS, SN_I_MINUS, SN_I_FOLDER, SN_I_SAVE,
    SN_I_EXPORT, SN_I_UNDO, SN_I_REDO, SN_I_EYE, SN_I_EYE_OFF, SN_I_SPEAKER,
    SN_I_MUTE, SN_I_LOCK, SN_I_UNLOCK, SN_I_ZOOM_IN, SN_I_ZOOM_OUT, SN_I_FIT,
    SN_I_LINK, SN_I_UNLINK, SN_I_INFO, SN_I_X, SN_I_CHECK, SN_I_SNAP,
    SN_I_UP, SN_I_DOWN, SN_I_CROP, SN_I_LOOP, SN_I_TEXT, SN_I_HELP
} sn_icon;

/* A filled triangle whichever way round its points are given.
 *
 * raylib culls one winding, and on screen - where y runs downwards - the
 * visible one is the order that comes out negative. Getting it wrong draws
 * nothing at all, silently, which is a bad way to spend an evening, so every
 * triangle in this program goes through here and it sorts the points itself. */
void sn_triangle(Vector2 a, Vector2 b, Vector2 c, Color col);

void sn_draw_icon(sn_icon which, Rectangle r, Color c);
/* Draw this icon as the pointer, instead of a system cursor. Cleared every
 * frame like everything else here. */
void sn_cursor_glyph(sn_ui *ui, sn_icon which);

/* Called by the window at the end of the frame: sets the system cursor, or
 * hides it and draws the glyph. */
void sn_cursor_apply(sn_ui *ui);

int sn_icon_button(sn_ui *ui, int id, Rectangle r, sn_icon which, int enabled,
                   int lit, const char *tip);

/* Off/on, filled when on. */
int sn_toggle(sn_ui *ui, int id, Rectangle r, const char *label, int on);

/* A horizontal slider. Returns nonzero on any frame the value moved; `v` is
 * 0..1 and the caller maps it. */
int sn_slider(sn_ui *ui, int id, Rectangle r, float *v);

/* A number the user drags or types. Returns nonzero when it changed. */
int sn_field(sn_ui *ui, int id, Rectangle r, std::string &text, const char *hint);

void sn_progress(Rectangle r, float frac, Color fill);

/* ------------------------------------------------------------------ *
 * Menu
 * ------------------------------------------------------------------ */

void sn_menu_open(sn_ui *ui, Vector2 at, const char **items, int count, int tag);
void sn_menu_close(sn_ui *ui);
/* The index chosen this frame, or -1. Call before sn_ui_overlay. */
int sn_menu_take(sn_ui *ui, int *tag);

/* ------------------------------------------------------------------ *
 * S. Tarr
 *
 * The mascot, drawn rather than loaded: a five-pointed star with a visor, from
 * the roster SVG. Drawn because a vector shape this simple is fewer lines than
 * the code to find, load and scale a file, and it stays crisp at whatever size
 * the window happens to be.
 * ------------------------------------------------------------------ */

/* Drawn, not loaded, and still drawn: this is S. Tarr the character, who
 * appears in the empty preview, in the export dialog and in the about window.
 * The program's *icon* is separate artwork - see tools/make-icons.sh - because
 * a five-pointed star is not a video editor. */
void sn_star(Vector2 center, float radius, float rotation);

#endif /* SN_GUI_H */
