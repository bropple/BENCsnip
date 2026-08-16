/*
 * BENCsnip GUI - theme and widget set.
 * See sn_gui.h for why these are hand-drawn.
 */

#include "sn_gui.h"
#include "sn_embed.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

Color SN_BG = {0x06, 0x0a, 0x05, 255};
Color SN_WELL = {0x0b, 0x0f, 0x0a, 255};
Color SN_PANEL = {0x18, 0x20, 0x10, 255};
Color SN_PANEL_HI = {0x22, 0x2c, 0x18, 255};
Color SN_BORDER = {0x2a, 0x3a, 0x1e, 255};
Color SN_TEXT = {0xcd, 0xea, 0xb0, 255};
Color SN_DIM = {0x8a, 0xa8, 0x78, 255};
/* What the status line says when it is explaining rather than reporting.
 *
 * It was SN_DIM, which is the colour for text you are not meant to read unless
 * you go looking - a unit beside a number, a hint under a slider. A tooltip is
 * the opposite: it is the answer to a question somebody is asking right now by
 * holding the pointer still, and on a near-black bar at nine pixels it was too
 * dark to read at a glance. Brighter than DIM and still short of TEXT, so a
 * message the program chose to tell you still reads as the louder of the two. */
Color SN_TIP = {0xa8, 0xcf, 0x84, 255};
Color SN_ACCENT = {0x78, 0xb9, 0x46, 255};
Color SN_EDGE = {0x3f, 0x5c, 0x28, 255};
Color SN_ALERT = {0xd8, 0x4a, 0x3a, 255};
Color SN_AMBER = {0xe8, 0xb2, 0x3d, 255};
Color SN_STAR = {0xee, 0xcb, 0x2e, 255};
Color SN_STAR_EDGE = {0xa3, 0x86, 0x1a, 255};
Color SN_VISOR = {0x9a, 0x9d, 0x94, 255};

/* P. Gon's blue and R. Triy's green, each with the 30-40% darker edge the
 * style guide pairs it with. */
Color SN_CLIP_V = {0x2d, 0x5c, 0x8c, 255};
Color SN_CLIP_V_HI = {0x3d, 0x7d, 0xbf, 255};
Color SN_CLIP_V_EDGE = {0x25, 0x4d, 0x75, 255};
Color SN_CLIP_A = {0x4a, 0x73, 0x2c, 255};
Color SN_CLIP_A_HI = {0x78, 0xb9, 0x46, 255};
Color SN_CLIP_A_EDGE = {0x3f, 0x5c, 0x28, 255};
/* Captions. A third hue rather than a shade of one of the other two, because
 * what a text clip is has to be readable at a glance across a timeline, and
 * amber is the one colour in this theme not already spoken for by video or
 * audio. */
Color SN_CLIP_T = {0x8c, 0x5c, 0x22, 255};
Color SN_CLIP_T_HI = {0xbf, 0x86, 0x35, 255};
Color SN_CLIP_T_EDGE = {0x75, 0x4b, 0x1c, 255};

/* ------------------------------------------------------------------ *
 * Fonts
 *
 * Loaded from the array in sn_embed.c rather than from a file. See
 * sn_embed.h for why, and NOTICE for the condition attached to it.
 * ------------------------------------------------------------------ */

/* Which glyphs to rasterise.
 *
 * Passing null here loads raylib's default set - the 95 printable ASCII
 * characters and nothing else - and this program shows text it did not write.
 * A codepoint the atlas has no glyph for is drawn as a question mark, so the
 * OFL appeared with one at the end of every line (it ships with CRLF endings)
 * and the NOTICE with one wherever it had an em dash.
 *
 * The line endings are a separate bug, fixed where the text is split. This is
 * the other half: ASCII, the Latin-1 supplement - accented names, the
 * copyright sign, degrees - and the punctuation block holding the dashes, the
 * curly quotes and the ellipsis. About three hundred glyphs, and Terminus has
 * every one of them. */
static const int *glyph_set(int *count)
{
    static int cps[512];
    static int n = 0;

    if (n == 0) {
        for (int c = 0x20; c <= 0x7E; c++) cps[n++] = c;      /* ASCII      */
        for (int c = 0xA0; c <= 0xFF; c++) cps[n++] = c;      /* Latin-1    */
        for (int c = 0x2010; c <= 0x2027; c++) cps[n++] = c;  /* – — ' ' " " … */
        cps[n++] = 0x20AC;                                    /* €          */
        cps[n++] = 0x2122;                                    /* ™          */
    }

    *count = n;
    return cps;
}

/* Point filtering, not bilinear. Terminus is a bitmap design; smoothing it is
 * how you get the mush this font exists to avoid. */
static Font load_at(int size, int *found)
{
    int n = 0;
    const int *cps = glyph_set(&n);
    Font f = LoadFontFromMemory(".ttf", SN_FONT_TTF, (int)SN_FONT_TTF_LEN, size,
                                (int *)cps, n);
    if (f.texture.id != 0 && f.glyphCount > 0) {
        SetTextureFilter(f.texture, TEXTURE_FILTER_POINT);
        *found = 1;
        return f;
    }
    return GetFontDefault();
}

void sn_ui_init(sn_ui *ui)
{
    memset(ui, 0, sizeof *ui);
    ui->tiny = load_at(SN_F_TINY, &ui->loaded);
    ui->small = load_at(SN_F_SMALL, &ui->loaded);
    ui->body = load_at(SN_F_BODY, &ui->loaded);
    ui->title = load_at(SN_F_TITLE, &ui->loaded);
    if (!ui->loaded)
        ui->tiny = ui->small = ui->body = ui->title = GetFontDefault();
}

void sn_ui_free(sn_ui *ui)
{
    if (!ui->loaded) return;
    UnloadFont(ui->tiny);
    UnloadFont(ui->small);
    UnloadFont(ui->body);
    UnloadFont(ui->title);
    ui->loaded = 0;
}

/* Which shape wins when two things ask in the same frame. Higher is more
 * specific: the edge of a clip is inside the clip, so both the resize and the
 * move get asked for and the resize is the one that means something. */
static int cursor_rank(int shape)
{
    switch (shape) {
    case MOUSE_CURSOR_RESIZE_EW:
    case MOUSE_CURSOR_RESIZE_NS: return 4;
    case MOUSE_CURSOR_RESIZE_ALL: return 3;
    case MOUSE_CURSOR_IBEAM: return 2;
    case MOUSE_CURSOR_POINTING_HAND: return 1;
    default: return 0;
    }
}

void sn_cursor(sn_ui *ui, int shape)
{
    if (cursor_rank(shape) > cursor_rank(ui->cursor)) ui->cursor = shape;
}

void sn_cursor_glyph(sn_ui *ui, sn_icon which)
{
    ui->cursorGlyph = (int)which;
}

void sn_cursor_apply(sn_ui *ui)
{
    /* Hidden and shown only when it changes. Asking every frame is a call
     * into the window system sixty times a second to say what it already
     * knows, and on some platforms a visible flicker. */
    static int hidden = 0;

    /* Nothing is claimed while the pointer is somewhere else.
     *
     * A cursor shape is set on the application, not on the rectangle it was
     * asked for, and on macOS it stays set: leave the window over a track
     * edge and the resize arrow follows you onto the desktop and stays there
     * until the program quits. The same goes for the hidden cursor the loop
     * glyph uses, which is worse - the pointer simply disappears.
     *
     * So the shape is handed back at the border. This is the only place that
     * knows the pointer left, because the panes stop being asked at all. */
    if (!IsCursorOnScreen() || !IsWindowFocused()) {
        if (hidden) { ShowCursor(); hidden = 0; }
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
        return;
    }

    if (ui->cursorGlyph >= 0) {
        if (!hidden) { HideCursor(); hidden = 1; }
        Vector2 m = GetMousePosition();
        Rectangle r = {m.x - 11, m.y - 11, 22, 22};
        /* A dark backing, because the pointer travels over a bright preview
         * and a phosphor-green glyph on a yellow bar is not a pointer. */
        DrawCircleV(Vector2{m.x, m.y}, 13, Color{0x06, 0x0a, 0x05, 190});
        sn_draw_icon((sn_icon)ui->cursorGlyph, r, SN_TEXT);
    } else {
        if (hidden) { ShowCursor(); hidden = 0; }
        SetMouseCursor(ui->cursor);
    }
}

void sn_ui_frame(sn_ui *ui)
{
    ui->tip[0] = 0;
    ui->suppress = 0;
    ui->cursor = MOUSE_CURSOR_DEFAULT;
    ui->cursorGlyph = -1;
    /* A drag ends when the button comes up, wherever the pointer is. Leaving
     * `active` set past the release is how a control ends up following the
     * mouse around with nothing held down. */
    if (ui->active && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) ui->active = 0;
}

int sn_ui_blocked(const sn_ui *ui)
{
    if (ui->suppress) return 1;
    return ui->menuOpen && CheckCollisionPointRec(GetMousePosition(), ui->menuRect);
}

int sn_double_click(sn_ui *ui, int id)
{
    const double now = GetTime();
    const int quick = (ui->lastId == id) && (now - ui->lastClick < 0.35);
    ui->lastId = id;
    ui->lastClick = now;
    return quick;
}

void sn_tip(sn_ui *ui, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->tip, sizeof ui->tip, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

static Font pick(const sn_ui *ui, int size)
{
    if (size <= SN_F_TINY) return ui->tiny;
    if (size <= SN_F_SMALL) return ui->small;
    if (size <= SN_F_BODY) return ui->body;
    return ui->title;
}

void sn_text(const sn_ui *ui, int size, const char *s, float x, float y, Color c)
{
    Vector2 p = {std::floor(x), std::floor(y)};
    DrawTextEx(pick(ui, size), s, p, (float)size, 0.0f, c);
}

/* Style-guide letter-spacing: 1px on labels and headings, never body text. */
void sn_text_spaced(const sn_ui *ui, int size, const char *s, float x, float y, Color c)
{
    Vector2 p = {std::floor(x), std::floor(y)};
    DrawTextEx(pick(ui, size), s, p, (float)size, 1.0f, c);
}

float sn_measure(const sn_ui *ui, int size, const char *s, float spacing)
{
    return MeasureTextEx(pick(ui, size), s, (float)size, spacing).x;
}

void sn_text_center(const sn_ui *ui, int size, const char *s, float cx, float y, Color c)
{
    sn_text(ui, size, s, cx - sn_measure(ui, size, s, 0.0f) * 0.5f, y, c);
}

void sn_text_clip(const sn_ui *ui, int size, const char *s, float x, float y,
                  float w, Color c)
{
    if (sn_measure(ui, size, s, 0.0f) <= w) { sn_text(ui, size, s, x, y, c); return; }

    /* Keep the end. "MVI_0043_final_final2.mp4" cut at the front still says
     * which file it is; cut at the back it says "MVI_0043_fin..." for every
     * clip in the bin. */
    char buf[256];
    size_t n = strlen(s);
    size_t cut = 0;
    for (cut = 0; cut < n; cut++) {
        snprintf(buf, sizeof buf, "...%s", s + cut);
        if (sn_measure(ui, size, buf, 0.0f) <= w) break;
    }
    sn_text(ui, size, buf, x, y, c);
}

/* ------------------------------------------------------------------ *
 * Chrome
 * ------------------------------------------------------------------ */

void sn_panel(Rectangle r, Color fill, Color border)
{
    if (r.height < 2 || r.width < 2) { DrawRectangleRec(r, fill); return; }
    const float round = (float)SN_RADIUS / (r.height < r.width ? r.height : r.width);
    DrawRectangleRounded(r, round, 4, fill);
    DrawRectangleRoundedLines(r, round, 4, border);
}

void sn_divider(float x, float y, float w)
{
    DrawRectangle((int)x, (int)y, (int)w, 1, SN_BORDER);
}

void sn_progress(Rectangle r, float frac, Color fill)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    DrawRectangleRec(r, SN_WELL);
    Rectangle f = {r.x, r.y, r.width * frac, r.height};
    DrawRectangleRec(f, fill);
    DrawRectangleLinesEx(r, 1, SN_BORDER);
}

static int button_common(sn_ui *ui, int id, Rectangle r, const char *label,
                         int enabled, Color fill, Color textCol)
{
    const Vector2 m = GetMousePosition();
    const int hot = enabled && !sn_ui_blocked(ui) && CheckCollisionPointRec(m, r);
    const int down = hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT);

    Color f = fill;
    if (!enabled) f = SN_PANEL;
    else if (down) f = SN_EDGE;
    else if (hot) f = SN_PANEL_HI;

    if (hot) sn_cursor(ui, MOUSE_CURSOR_POINTING_HAND);

    sn_panel(r, f, enabled ? SN_BORDER : SN_PANEL_HI);

    const Color tc = enabled ? textCol : SN_EDGE;
    const float tw = sn_measure(ui, SN_F_SMALL, label, 1.0f);
    sn_text_spaced(ui, SN_F_SMALL, label, r.x + (r.width - tw) * 0.5f,
                   r.y + (r.height - SN_F_SMALL) * 0.5f, tc);

    return hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

int sn_button(sn_ui *ui, int id, Rectangle r, const char *label, int enabled)
{
    return button_common(ui, id, r, label, enabled, SN_PANEL, SN_TEXT);
}

int sn_button_lit(sn_ui *ui, int id, Rectangle r, const char *label, int lit)
{
    if (!lit) return button_common(ui, id, r, label, 1, SN_PANEL, SN_TEXT);
    return button_common(ui, id, r, label, 1, SN_ACCENT, SN_BG);
}

int sn_toggle(sn_ui *ui, int id, Rectangle r, const char *label, int on)
{
    return button_common(ui, id, r, label, 1, on ? SN_ACCENT : SN_PANEL,
                         on ? SN_BG : SN_DIM);
}

/* ------------------------------------------------------------------ *
 * Icons
 *
 * Drawn from primitives at whatever size the rectangle is. A sixteen-pixel
 * bitmap would be blurry on a scaled display and a second thing to keep in
 * step with the palette; a triangle is three points.
 * ------------------------------------------------------------------ */

void sn_triangle(Vector2 a, Vector2 b, Vector2 c, Color col)
{
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross < 0) DrawTriangle(a, b, c, col);
    else DrawTriangle(a, c, b, col);
}

void sn_draw_icon(sn_icon which, Rectangle r, Color c)
{
    /* Everything is drawn inside a unit square from the middle out, so the
     * glyphs are the same weight beside each other whatever size they are. */
    const float s = (r.width < r.height ? r.width : r.height) * 0.5f;
    const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;
    const float th = std::floor(s * 0.28f) < 2 ? 2.0f : std::floor(s * 0.28f);

    auto V = [&](float x, float y) { return Vector2{cx + x * s, cy + y * s}; };

    auto tri = [&](Vector2 a, Vector2 b, Vector2 d) { sn_triangle(a, b, d, c); };
    auto bar = [&](float x, float y, float w, float h) {
        DrawRectangleRec(Rectangle{cx + x * s, cy + y * s, w * s, h * s}, c);
    };
    auto line = [&](float x1, float y1, float x2, float y2, float w) {
        DrawLineEx(V(x1, y1), V(x2, y2), w, c);
    };

    switch (which) {
    case SN_I_PLAY:
        tri(V(-0.5f, -0.7f), V(-0.5f, 0.7f), V(0.7f, 0.0f));
        break;
    case SN_I_PAUSE:
        bar(-0.55f, -0.65f, 0.35f, 1.3f);
        bar(0.2f, -0.65f, 0.35f, 1.3f);
        break;
    case SN_I_STOP:
        bar(-0.6f, -0.6f, 1.2f, 1.2f);
        break;
    case SN_I_START:
        bar(-0.7f, -0.65f, 0.25f, 1.3f);
        tri(V(0.7f, -0.7f), V(0.7f, 0.7f), V(-0.35f, 0.0f));
        break;
    case SN_I_END:
        bar(0.45f, -0.65f, 0.25f, 1.3f);
        tri(V(-0.7f, 0.7f), V(-0.7f, -0.7f), V(0.35f, 0.0f));
        break;
    case SN_I_PREV:
        tri(V(0.6f, -0.7f), V(0.6f, 0.7f), V(-0.1f, 0.0f));
        tri(V(0.0f, -0.7f), V(0.0f, 0.7f), V(-0.7f, 0.0f));
        break;
    case SN_I_NEXT:
        tri(V(-0.6f, 0.7f), V(-0.6f, -0.7f), V(0.1f, 0.0f));
        tri(V(0.0f, 0.7f), V(0.0f, -0.7f), V(0.7f, 0.0f));
        break;
    case SN_I_SPLIT:
        /* A cut: two blocks with a dashed line down the gap between them. */
        bar(-0.85f, -0.6f, 0.6f, 1.2f);
        bar(0.25f, -0.6f, 0.6f, 1.2f);
        for (float y = -0.85f; y < 0.85f; y += 0.45f) bar(-0.08f, y, 0.16f, 0.28f);
        break;
    case SN_I_TRASH:
        bar(-0.55f, -0.5f, 1.1f, 0.16f);
        bar(-0.2f, -0.75f, 0.4f, 0.16f);
        bar(-0.45f, -0.3f, 0.9f, 1.05f);
        break;
    case SN_I_PLUS:
        bar(-0.7f, -0.12f, 1.4f, 0.24f);
        bar(-0.12f, -0.7f, 0.24f, 1.4f);
        break;
    case SN_I_MINUS:
        bar(-0.7f, -0.12f, 1.4f, 0.24f);
        break;
    case SN_I_FOLDER:
        bar(-0.75f, -0.5f, 0.6f, 0.2f);
        bar(-0.75f, -0.35f, 1.5f, 1.05f);
        break;
    case SN_I_SAVE:
        /* Down into a tray. A floppy disk is the conventional glyph and is
         * unreadable at this size - and increasingly unrecognisable at any
         * size. */
        line(0.0f, -0.8f, 0.0f, 0.25f, th);
        tri(V(0.45f, 0.05f), V(-0.45f, 0.05f), V(0.0f, 0.6f));
        bar(-0.75f, 0.6f, 1.5f, 0.2f);
        break;
    case SN_I_EXPORT:
        /* Out of a tray and away. */
        line(0.0f, 0.6f, 0.0f, -0.55f, th);
        tri(V(-0.45f, -0.3f), V(0.45f, -0.3f), V(0.0f, -0.85f));
        bar(-0.75f, 0.6f, 1.5f, 0.2f);
        break;
    case SN_I_UNDO:
    case SN_I_REDO: {
        /* Half a ring with a head on the end it points at: an arrow that has
         * come back round on itself. */
        const float rad = 0.55f;
        DrawRing(Vector2{cx, cy + 0.1f * s}, rad * s - th * 0.5f, rad * s + th * 0.5f,
                 180.0f, 360.0f, 28, c);
        const float ax = (which == SN_I_UNDO ? -rad : rad);
        tri(V(ax - 0.3f, 0.1f), V(ax + 0.3f, 0.1f), V(ax, 0.7f));
        break;
    }
    case SN_I_EYE:
    case SN_I_EYE_OFF:
        tri(V(-0.85f, 0.0f), V(0.0f, -0.5f), V(0.85f, 0.0f));
        tri(V(-0.85f, 0.0f), V(0.85f, 0.0f), V(0.0f, 0.5f));
        DrawCircleV(Vector2{cx, cy}, 0.22f * s, SN_PANEL);
        if (which == SN_I_EYE_OFF) {
            DrawLineEx(V(-0.85f, -0.7f), V(0.85f, 0.7f), th + 2.0f, SN_PANEL);
            line(-0.8f, -0.65f, 0.8f, 0.65f, th);
        }
        break;
    case SN_I_SPEAKER:
    case SN_I_MUTE:
        bar(-0.75f, -0.25f, 0.35f, 0.5f);
        tri(V(-0.15f, -0.75f), V(-0.4f, 0.0f), V(-0.15f, 0.75f));
        if (which == SN_I_SPEAKER) {
            DrawRing(Vector2{cx - 0.15f * s, cy}, 0.45f * s, 0.45f * s + th * 0.5f,
                     -55, 55, 16, c);
            DrawRing(Vector2{cx - 0.15f * s, cy}, 0.8f * s, 0.8f * s + th * 0.5f,
                     -55, 55, 16, c);
        } else {
            line(0.15f, -0.5f, 0.8f, 0.5f, th * 0.8f);
            line(0.8f, -0.5f, 0.15f, 0.5f, th * 0.8f);
        }
        break;
    case SN_I_LOCK:
    case SN_I_UNLOCK:
        DrawRing(Vector2{cx + (which == SN_I_LOCK ? 0.0f : 0.5f) * s, cy - 0.15f * s},
                 0.4f * s - th * 0.5f, 0.4f * s + th * 0.5f, 180, 360, 20, c);
        bar(-0.65f, -0.15f, 1.3f, 0.85f);
        break;
    case SN_I_ZOOM_IN:
    case SN_I_ZOOM_OUT:
        DrawRing(Vector2{cx - 0.15f * s, cy - 0.15f * s}, 0.5f * s, 0.5f * s + th * 0.7f,
                 0, 360, 24, c);
        line(0.25f, 0.25f, 0.75f, 0.75f, th);
        bar(-0.45f, -0.2f, 0.6f, 0.12f);
        if (which == SN_I_ZOOM_IN) bar(-0.21f, -0.45f, 0.12f, 0.6f);
        break;
    case SN_I_FIT:
        DrawRectangleLinesEx(Rectangle{cx - 0.75f * s, cy - 0.5f * s, 1.5f * s, 1.0f * s},
                             th * 0.6f, c);
        line(-0.35f, 0.0f, 0.35f, 0.0f, th * 0.7f);
        tri(V(-0.75f, 0.0f), V(-0.35f, 0.25f), V(-0.35f, -0.25f));
        tri(V(0.75f, 0.0f), V(0.35f, -0.25f), V(0.35f, 0.25f));
        break;
    case SN_I_LINK:
    case SN_I_UNLINK:
        DrawRing(Vector2{cx - 0.35f * s, cy}, 0.32f * s, 0.32f * s + th * 0.7f, 0, 360, 20, c);
        DrawRing(Vector2{cx + 0.35f * s, cy}, 0.32f * s, 0.32f * s + th * 0.7f, 0, 360, 20, c);
        if (which == SN_I_LINK) bar(-0.2f, -0.09f, 0.4f, 0.18f);
        else line(-0.15f, -0.55f, 0.15f, 0.55f, th * 0.8f);
        break;
    case SN_I_UP:
        tri(V(-0.6f, 0.35f), V(0.6f, 0.35f), V(0.0f, -0.45f));
        break;
    case SN_I_DOWN:
        tri(V(-0.6f, -0.35f), V(0.6f, -0.35f), V(0.0f, 0.45f));
        break;
    case SN_I_LOOP:
        /* Three quarters of a ring with a head on the end: the arrow that
         * every program in the world draws for "again". */
        DrawRing(Vector2{cx, cy}, 0.5f * s - th * 0.5f, 0.5f * s + th * 0.5f, 20, 320,
                 28, c);
        tri(V(0.28f, -0.72f), V(0.92f, -0.52f), V(0.42f, -0.06f));
        break;
    case SN_I_HELP:
        /* A question mark: the hook, and the dot under it. Drawn from a ring
         * with a gap rather than from a curve, because there is no curve
         * primitive here and three quarters of a ring is the shape of the
         * hook anyway. */
        DrawRing(Vector2{cx, cy - 0.34f * s}, 0.40f * s - th * 0.5f,
                 0.40f * s + th * 0.5f, 145, 400, 28, c);
        bar(-0.11f, 0.04f, 0.22f, 0.30f);
        bar(-0.11f, 0.50f, 0.22f, 0.24f);
        break;
    case SN_I_TEXT:
        /* A serifed capital T: the crossbar, the stem, and a foot. Plain
         * enough at 26 pixels to read as a letter rather than as a shape, and
         * the serifs are what stop it reading as a plus sign. */
        bar(-0.75f, -0.7f, 1.5f, 0.22f);
        bar(-0.11f, -0.7f, 0.22f, 1.4f);
        bar(-0.42f, 0.48f, 0.84f, 0.22f);
        break;
    case SN_I_CROP:
        /* Two overlapping corners, which is what a crop tool has looked like
         * since before any of this. */
        bar(-0.75f, -0.35f, 0.2f, 1.1f);
        bar(-0.75f, 0.55f, 1.5f, 0.2f);
        bar(0.55f, -0.75f, 0.2f, 1.1f);
        bar(-0.75f, -0.75f, 1.5f, 0.2f);
        break;
    case SN_I_INFO:
        DrawRing(Vector2{cx, cy}, 0.7f * s, 0.7f * s + th * 0.7f, 0, 360, 24, c);
        bar(-0.1f, -0.45f, 0.2f, 0.2f);
        bar(-0.1f, -0.12f, 0.2f, 0.6f);
        break;
    case SN_I_X:
        line(-0.6f, -0.6f, 0.6f, 0.6f, th);
        line(0.6f, -0.6f, -0.6f, 0.6f, th);
        break;
    case SN_I_CHECK:
        line(-0.65f, 0.0f, -0.2f, 0.5f, th);
        line(-0.2f, 0.5f, 0.65f, -0.55f, th);
        break;
    case SN_I_SNAP:
        /* A magnet: a half ring with two legs coming down off its ends. */
        DrawRing(Vector2{cx, cy + 0.1f * s}, 0.3f * s, 0.65f * s, 180, 360, 24, c);
        bar(-0.65f, 0.1f, 0.35f, 0.6f);
        bar(0.3f, 0.1f, 0.35f, 0.6f);
        break;
    }
}

int sn_icon_button(sn_ui *ui, int id, Rectangle r, sn_icon which, int enabled,
                   int lit, const char *tip)
{
    const Vector2 m = GetMousePosition();
    const int hot = enabled && !sn_ui_blocked(ui) && CheckCollisionPointRec(m, r);
    const int down = hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT);

    Color fill = SN_PANEL;
    if (lit) fill = SN_ACCENT;
    else if (down) fill = SN_EDGE;
    else if (hot) fill = SN_PANEL_HI;

    if (hot) sn_cursor(ui, MOUSE_CURSOR_POINTING_HAND);
    sn_panel(r, fill, enabled ? (hot || lit ? SN_ACCENT : SN_BORDER) : SN_PANEL_HI);

    Color ink = lit ? SN_BG : (enabled ? (hot ? SN_TEXT : SN_DIM) : SN_EDGE);
    /* The glyph is drawn from the middle of this box outwards and reaches a
     * little past it, so the box is smaller than it looks - 0.56 of the
     * button here is a glyph filling about three quarters of it, which is
     * what reads at sixteen pixels. */
    Rectangle g = {r.x + r.width * 0.22f, r.y + r.height * 0.22f,
                   r.width * 0.56f, r.height * 0.56f};
    sn_draw_icon(which, g, ink);

    if (hot && tip) sn_tip(ui, "%s", tip);
    return hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/* ------------------------------------------------------------------ *
 * Slider
 * ------------------------------------------------------------------ */

int sn_slider(sn_ui *ui, int id, Rectangle r, float *v)
{
    const Vector2 m = GetMousePosition();
    const int hot = !sn_ui_blocked(ui) && CheckCollisionPointRec(m, r);

    if (hot || ui->active == id) sn_cursor(ui, MOUSE_CURSOR_RESIZE_EW);
    if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui->active = id;

    int changed = 0;
    if (ui->active == id) {
        float t = (m.x - r.x) / (r.width > 1 ? r.width : 1);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        if (t != *v) { *v = t; changed = 1; }
    }

    const float y = r.y + r.height * 0.5f;
    DrawRectangle((int)r.x, (int)(y - 1), (int)r.width, 2, SN_EDGE);
    DrawRectangle((int)r.x, (int)(y - 1), (int)(r.width * *v), 2, SN_ACCENT);

    const float hx = r.x + r.width * *v;
    Rectangle knob = {hx - 3, r.y + 2, 6, r.height - 4};
    DrawRectangleRec(knob, (hot || ui->active == id) ? SN_TEXT : SN_DIM);

    return changed;
}

/* ------------------------------------------------------------------ *
 * Text field
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Text fields
 *
 * A box you can type in is a control everybody has used ten thousand times
 * before they get here, and the whole of its behaviour is muscle memory:
 * click where you want the caret, drag over a word to select it, shift-arrow
 * to extend, control-C, control-V, right-click for a menu. A field that
 * accepts characters and nothing else is a field that will be fought with
 * every time somebody has to correct the middle of a path.
 * ------------------------------------------------------------------ */

/* Menus opened by a field carry a tag of their own, well clear of the ones
 * the panes use, so a field can answer its own menu without the window's
 * dispatcher ever seeing it. */
enum { SN_FIELD_MENU = 500000 };

static int field_is_word(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '_';
}

/* Where the caret goes for a click at this x. Nearest gap between two
 * characters rather than the one under the pointer, which is what makes
 * clicking just past the end of a word land after it. */
static int field_caret_at(sn_ui *ui, const std::string &text, float left, float x)
{
    int best = 0;
    float bestD = 1e9f;
    for (int i = 0; i <= (int)text.size(); i++) {
        const float w = sn_measure(ui, SN_F_SMALL, text.substr(0, i).c_str(), 0.0f);
        const float d = std::fabs(left + w - x);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

static void field_clamp(sn_ui *ui, const std::string &text)
{
    const int n = (int)text.size();
    if (ui->caret > n) ui->caret = n;
    if (ui->caret < 0) ui->caret = 0;
    if (ui->anchor > n) ui->anchor = n;
    if (ui->anchor < 0) ui->anchor = 0;
}

/* The selected range, low end first. Empty when the two ends agree. */
static void field_range(const sn_ui *ui, int *from, int *to)
{
    *from = ui->caret < ui->anchor ? ui->caret : ui->anchor;
    *to   = ui->caret < ui->anchor ? ui->anchor : ui->caret;
}

static int field_erase_selection(sn_ui *ui, std::string &text)
{
    int a, b;
    field_range(ui, &a, &b);
    if (a == b) return 0;
    text.erase(text.begin() + a, text.begin() + b);
    ui->caret = ui->anchor = a;
    return 1;
}

/* Insert, replacing whatever was selected. Control characters are dropped
 * rather than pasted: a filename with a newline in it is a filename that
 * fails to open somewhere far away from here. */
static int field_insert(sn_ui *ui, std::string &text, const char *s)
{
    int changed = field_erase_selection(ui, text);
    for (const char *p = s; p && *p; p++) {
        if (*p < 32 || (unsigned char)*p >= 127) continue;
        if (text.size() >= 512) break;
        text.insert(text.begin() + ui->caret, *p);
        ui->caret++;
        changed = 1;
    }
    ui->anchor = ui->caret;
    return changed;
}

int sn_field(sn_ui *ui, int id, Rectangle r, std::string &text, const char *hint)
{
    const Vector2 m = GetMousePosition();
    const int hot = !sn_ui_blocked(ui) && CheckCollisionPointRec(m, r);
    if (hot) sn_cursor(ui, MOUSE_CURSOR_IBEAM);

    int changed = 0;
    const float pad = 6;
    const float inner = r.width - pad * 2;

    /* The horizontal scroll has to be known before the mouse can be turned
     * into a caret position, and it depends on where the caret already is:
     * a path longer than the box is the normal case here, not the exception. */
    if (ui->focus == id) field_clamp(ui, text);
    const float caretW =
        sn_measure(ui, SN_F_SMALL, text.substr(0, ui->focus == id ? ui->caret : 0).c_str(),
                   0.0f);
    const float shift = (ui->focus == id && caretW > inner) ? caretW - inner : 0.0f;
    const float left = r.x + pad - shift;

    /* --- the mouse --- */
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !sn_ui_blocked(ui)) {
        if (hot) {
            const int wasFocused = ui->focus == id;
            ui->focus = id;
            if (!wasFocused) { ui->caret = (int)text.size(); ui->anchor = ui->caret; }

            const int at = field_caret_at(ui, text, left, m.x);

            if (sn_double_click(ui, 400000 + id)) {
                /* The word under the pointer, or everything if that is not a
                 * word - which is what makes double-clicking a number or a
                 * bare filename select the lot. */
                int a = at, b = at;
                while (a > 0 && field_is_word(text[a - 1])) a--;
                while (b < (int)text.size() && field_is_word(text[b])) b++;
                if (a == b) { a = 0; b = (int)text.size(); }
                ui->anchor = a;
                ui->caret = b;
            } else {
                ui->caret = ui->anchor = at;
                ui->fieldDrag = 1;
            }
        } else if (ui->focus == id) {
            ui->focus = 0;
            ui->fieldDrag = 0;
        }
    }

    if (ui->fieldDrag && ui->focus == id) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) ui->caret = field_caret_at(ui, text, left, m.x);
        else ui->fieldDrag = 0;
    }

    if (hot && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !sn_ui_blocked(ui)) {
        ui->focus = id;
        field_clamp(ui, text);
        static const char *items[] = {"cut", "copy", "paste", "-", "select all"};
        sn_menu_open(ui, m, items, 5, SN_FIELD_MENU + id);
    }

    /* Its own menu, answered here rather than by the window: a generic widget
     * has no way to reach into the application's dispatcher, and the
     * application has no business knowing what a text field's menu says. */
    if (ui->menuOpen && ui->menuTag == SN_FIELD_MENU + id) {
        int tag = 0;
        const int pick = sn_menu_take(ui, &tag);
        if (pick >= 0) {
            int a, b;
            field_range(ui, &a, &b);
            switch (pick) {
            case 0:
                if (a != b) {
                    SetClipboardText(text.substr(a, b - a).c_str());
                    changed |= field_erase_selection(ui, text);
                }
                break;
            case 1:
                if (a != b) SetClipboardText(text.substr(a, b - a).c_str());
                break;
            case 2: {
                const char *clip = GetClipboardText();
                if (clip && *clip) changed |= field_insert(ui, text, clip);
                break;
            }
            case 4:
                ui->anchor = 0;
                ui->caret = (int)text.size();
                break;
            default: break;
            }
        }
    }

    const int focused = ui->focus == id;

    /* --- the keyboard --- */
    const int caretWas = ui->caret;

    if (focused) {
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                          IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
        const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

        auto press = [](int k) { return IsKeyPressed(k) || IsKeyPressedRepeat(k); };

        /* After any movement: shift keeps the anchor where it was and drags
         * the selection with the caret, and no shift collapses it. */
        auto moved = [&](bool sh) { if (!sh) ui->anchor = ui->caret; };

        if (ctrl) {
            int a, b;
            field_range(ui, &a, &b);

            if (IsKeyPressed(KEY_A)) { ui->anchor = 0; ui->caret = (int)text.size(); }
            if (IsKeyPressed(KEY_C) && a != b) SetClipboardText(text.substr(a, b - a).c_str());
            if (IsKeyPressed(KEY_X) && a != b) {
                SetClipboardText(text.substr(a, b - a).c_str());
                changed |= field_erase_selection(ui, text);
            }
            if (IsKeyPressed(KEY_V)) {
                const char *clip = GetClipboardText();
                if (clip && *clip) changed |= field_insert(ui, text, clip);
            }

            /* Word at a time. Over the run of separators first, then the run
             * of word characters, which is where every other text box stops. */
            if (press(KEY_LEFT)) {
                while (ui->caret > 0 && !field_is_word(text[ui->caret - 1])) ui->caret--;
                while (ui->caret > 0 && field_is_word(text[ui->caret - 1])) ui->caret--;
                moved(shift);
            }
            if (press(KEY_RIGHT)) {
                const int n = (int)text.size();
                while (ui->caret < n && !field_is_word(text[ui->caret])) ui->caret++;
                while (ui->caret < n && field_is_word(text[ui->caret])) ui->caret++;
                moved(shift);
            }
        } else {
            int ch;
            while ((ch = GetCharPressed()) != 0) {
                if (ch >= 32 && ch < 127) {
                    const char s[2] = {(char)ch, 0};
                    changed |= field_insert(ui, text, s);
                }
            }

            if (press(KEY_LEFT)) {
                int a, b;
                field_range(ui, &a, &b);
                /* A selection collapses to its near end rather than moving
                 * the caret one further, which is what every other text box
                 * does and what the hand expects. */
                if (a != b && !shift) ui->caret = a;
                else if (ui->caret > 0) ui->caret--;
                moved(shift);
            }
            if (press(KEY_RIGHT)) {
                int a, b;
                field_range(ui, &a, &b);
                if (a != b && !shift) ui->caret = b;
                else if (ui->caret < (int)text.size()) ui->caret++;
                moved(shift);
            }
            if (press(KEY_HOME)) { ui->caret = 0; moved(shift); }
            if (press(KEY_END)) { ui->caret = (int)text.size(); moved(shift); }

            if (press(KEY_BACKSPACE)) {
                if (!field_erase_selection(ui, text)) {
                    if (ui->caret > 0) {
                        text.erase(text.begin() + (ui->caret - 1));
                        ui->caret--;
                        ui->anchor = ui->caret;
                        changed = 1;
                    }
                } else {
                    changed = 1;
                }
            }
            if (press(KEY_DELETE)) {
                if (!field_erase_selection(ui, text)) {
                    if (ui->caret < (int)text.size()) {
                        text.erase(text.begin() + ui->caret);
                        changed = 1;
                    }
                } else {
                    changed = 1;
                }
            }
        }

        field_clamp(ui, text);
        if (ui->caret != caretWas || changed) ui->caretLive = GetTime();
    }

    /* --- drawing --- */
    sn_panel(r, SN_WELL, focused ? SN_ACCENT : SN_BORDER);

    const float ty = r.y + (r.height - SN_F_SMALL) * 0.5f;

    if (text.empty() && hint && !focused) {
        sn_text(ui, SN_F_SMALL, hint, r.x + pad, ty, SN_EDGE);
        return changed;
    }

    BeginScissorMode((int)(r.x + 1), (int)r.y, (int)(r.width - 2), (int)r.height);

    if (focused) {
        int a, b;
        field_range(ui, &a, &b);
        if (a != b) {
            const float xa = left + sn_measure(ui, SN_F_SMALL, text.substr(0, a).c_str(), 0.0f);
            const float xb = left + sn_measure(ui, SN_F_SMALL, text.substr(0, b).c_str(), 0.0f);
            DrawRectangleRec(Rectangle{xa, ty - 2, xb - xa, (float)SN_F_SMALL + 4},
                             Color{SN_ACCENT.r, SN_ACCENT.g, SN_ACCENT.b, 90});
        }
    }

    sn_text(ui, SN_F_SMALL, text.c_str(), left, ty, SN_TEXT);

    if (focused) {
        const float cx = left + sn_measure(ui, SN_F_SMALL,
                                           text.substr(0, ui->caret).c_str(), 0.0f);
        /* Solid while anything is happening to it, blinking once it is left
         * alone - see sn_ui::caretLive. */
        const bool busy = ui->fieldDrag || GetTime() - ui->caretLive < 0.6;
        if (busy || std::fmod(GetTime(), 1.0) < 0.5)
            DrawRectangle((int)cx, (int)ty, 1, SN_F_SMALL, SN_TEXT);
    }

    EndScissorMode();
    return changed;
}

/* ------------------------------------------------------------------ *
 * S. Tarr
 * ------------------------------------------------------------------ */

/* The ratios the roster SVG is built from, kept in one place so the icon and
 * the on-screen mark cannot come apart. */
static const float STAR_INNER = 0.421f;
static const float STAR_VIS_W = 1.158f, STAR_VIS_H = 0.358f, STAR_VIS_Y = -0.053f;
static const float STAR_STR_W = 0.737f, STAR_STR_H = 0.168f, STAR_STR_Y = 0.042f;


static void star_points(Vector2 *pts, Vector2 c, float r, float rot)
{
    for (int i = 0; i < 10; i++) {
        const float a = rot + (-90.0f + (float)i * 36.0f) * DEG2RAD;
        const float rr = r * ((i % 2 == 0) ? 1.0f : STAR_INNER);
        pts[i].x = c.x + std::cos(a) * rr;
        pts[i].y = c.y + std::sin(a) * rr;
    }
}



void sn_star(Vector2 center, float radius, float rotation)
{
    Vector2 pts[10];
    star_points(pts, center, radius, rotation);

    /* Counter-clockwise on screen means walking the points backwards, since y
     * runs down; raylib culls the other winding. */
    Vector2 fan[12];
    fan[0] = center;
    for (int i = 0; i < 10; i++) fan[1 + i] = pts[9 - i];
    fan[11] = pts[9];
    DrawTriangleFan(fan, 12, SN_STAR);

    for (int i = 0; i < 10; i++)
        DrawLineEx(pts[i], pts[(i + 1) % 10], radius * 0.03f + 1.0f, SN_STAR_EDGE);

    Rectangle visor = {center.x - radius * STAR_VIS_W * 0.5f, center.y + radius * STAR_VIS_Y,
                       radius * STAR_VIS_W, radius * STAR_VIS_H};
    Rectangle strip = {center.x - radius * STAR_STR_W * 0.5f, center.y + radius * STAR_STR_Y,
                       radius * STAR_STR_W, radius * STAR_STR_H};
    DrawRectangleRec(visor, SN_VISOR);
    DrawRectangleRec(strip, SN_ALERT);
}

/* ------------------------------------------------------------------ *
 * Menu
 * ------------------------------------------------------------------ */

enum { MENU_ROW = 22 };

void sn_menu_open(sn_ui *ui, Vector2 at, const char **items, int count, int tag)
{
    float w = 120.0f;
    for (int i = 0; i < count; i++) {
        const float tw = sn_measure(ui, SN_F_SMALL, items[i], 1.0f) + 28.0f;
        if (tw > w) w = tw;
    }

    Rectangle r = {at.x, at.y, w, (float)(count * MENU_ROW) + 8.0f};

    /* Keep it on screen: a menu opened near the bottom right otherwise runs
     * off, and its last item is the one nobody can reach. */
    if (r.x + r.width > GetScreenWidth()) r.x = GetScreenWidth() - r.width - 4;
    if (r.y + r.height > GetScreenHeight()) r.y = GetScreenHeight() - r.height - 4;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;

    ui->menuOpen = 1;
    ui->menuRect = r;
    ui->menuItems = items;
    ui->menuCount = count;
    ui->menuTag = tag;
    ui->menuHover = -1;
    ui->menuFresh = 1;
}

void sn_menu_close(sn_ui *ui)
{
    ui->menuOpen = 0;
    ui->menuCount = 0;
}

int sn_menu_take(sn_ui *ui, int *tag)
{
    if (!ui->menuOpen) return -1;

    const Vector2 m = GetMousePosition();
    const Rectangle r = ui->menuRect;

    ui->menuHover = -1;
    if (CheckCollisionPointRec(m, r)) {
        int i = (int)((m.y - r.y - 4.0f) / MENU_ROW);
        if (i >= 0 && i < ui->menuCount) ui->menuHover = i;
    }

    /* A menu opened by a button press would otherwise be chosen from by that
     * same press, which is still "released" later in the same frame. */
    if (ui->menuFresh) { ui->menuFresh = 0; return -1; }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (ui->menuHover >= 0) {
            if (tag) *tag = ui->menuTag;
            const int i = ui->menuHover;
            sn_menu_close(ui);
            return i;
        }
        sn_menu_close(ui);
    }
    if (IsKeyPressed(KEY_ESCAPE)) sn_menu_close(ui);

    return -1;
}

void sn_ui_overlay(sn_ui *ui)
{
    if (!ui->menuOpen) return;

    const Rectangle r = ui->menuRect;
    DrawRectangle((int)r.x + 2, (int)r.y + 2, (int)r.width, (int)r.height,
                  Color{0, 0, 0, 120});
    sn_panel(r, SN_PANEL, SN_ACCENT);

    for (int i = 0; i < ui->menuCount; i++) {
        Rectangle row = {r.x + 2, r.y + 4 + (float)i * MENU_ROW, r.width - 4, MENU_ROW};
        if (i == ui->menuHover) DrawRectangleRec(row, SN_EDGE);

        const char *s = ui->menuItems[i];
        /* A leading dash is a separator, not an item you can pick. */
        if (s[0] == '-' && s[1] == 0) {
            sn_divider(row.x + 6, row.y + MENU_ROW * 0.5f, row.width - 12);
        } else {
            sn_text_spaced(ui, SN_F_SMALL, s, row.x + 10,
                           row.y + (MENU_ROW - SN_F_SMALL) * 0.5f,
                           i == ui->menuHover ? SN_TEXT : SN_DIM);
        }
    }
}
