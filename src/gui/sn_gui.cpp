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

/* ------------------------------------------------------------------ *
 * Fonts
 *
 * Loaded from the array in sn_embed.c rather than from a file. See
 * sn_embed.h for why, and NOTICE for the condition attached to it.
 * ------------------------------------------------------------------ */

/* Point filtering, not bilinear. Terminus is a bitmap design; smoothing it is
 * how you get the mush this font exists to avoid. */
static Font load_at(int size, int *found)
{
    Font f = LoadFontFromMemory(".ttf", SN_FONT_TTF, (int)SN_FONT_TTF_LEN, size, 0, 0);
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

void sn_ui_frame(sn_ui *ui)
{
    ui->tip[0] = 0;
    ui->hot = 0;
    ui->suppress = 0;
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

void sn_draw_icon(sn_icon which, Rectangle r, Color c)
{
    /* Everything is drawn inside a unit square from the middle out, so the
     * glyphs are the same weight beside each other whatever size they are. */
    const float s = (r.width < r.height ? r.width : r.height) * 0.5f;
    const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;
    const float th = std::floor(s * 0.28f) < 2 ? 2.0f : std::floor(s * 0.28f);

    auto V = [&](float x, float y) { return Vector2{cx + x * s, cy + y * s}; };

    /* raylib culls one winding, and on screen - where y runs downwards - the
     * visible one is the order that comes out negative here. Getting it wrong
     * draws nothing at all, silently, which is a bad way to spend an evening;
     * so the three points go through this and it puts them in the right order
     * itself. */
    auto tri = [&](Vector2 a, Vector2 b, Vector2 d) {  /* always in `c` */
        const float cross = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
        if (cross < 0) DrawTriangle(a, b, d, c);
        else DrawTriangle(a, d, b, c);
    };
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
        DrawRing(Vector2{cx - 0.3f * s, cy}, 0.35f * s, 0.35f * s + th * 0.7f, 0, 360, 20, c);
        DrawRing(Vector2{cx + 0.3f * s, cy}, 0.35f * s, 0.35f * s + th * 0.7f, 0, 360, 20, c);
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

int sn_field(sn_ui *ui, int id, Rectangle r, std::string &text, const char *hint)
{
    const Vector2 m = GetMousePosition();
    const int hot = !sn_ui_blocked(ui) && CheckCollisionPointRec(m, r);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !sn_ui_blocked(ui)) {
        if (hot) { ui->focus = id; ui->caret = (int)text.size(); }
        else if (ui->focus == id) ui->focus = 0;
    }

    int changed = 0;
    const int focused = ui->focus == id;

    if (focused) {
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            if (ch >= 32 && ch < 127 && text.size() < 512) {
                text.insert(text.begin() + ui->caret, (char)ch);
                ui->caret++;
                changed = 1;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (ui->caret > 0) {
                text.erase(text.begin() + (ui->caret - 1));
                ui->caret--;
                changed = 1;
            }
        }
        if (IsKeyPressed(KEY_DELETE) && ui->caret < (int)text.size()) {
            text.erase(text.begin() + ui->caret);
            changed = 1;
        }
        if (IsKeyPressed(KEY_LEFT) && ui->caret > 0) ui->caret--;
        if (IsKeyPressed(KEY_RIGHT) && ui->caret < (int)text.size()) ui->caret++;
        if (IsKeyPressed(KEY_HOME)) ui->caret = 0;
        if (IsKeyPressed(KEY_END)) ui->caret = (int)text.size();
        if (ui->caret > (int)text.size()) ui->caret = (int)text.size();
    }

    sn_panel(r, SN_WELL, focused ? SN_ACCENT : SN_BORDER);

    const float pad = 6;
    const float ty = r.y + (r.height - SN_F_SMALL) * 0.5f;

    if (text.empty() && hint && !focused) {
        sn_text(ui, SN_F_SMALL, hint, r.x + pad, ty, SN_EDGE);
    } else {
        /* Scroll so the caret stays visible: a path longer than the box is
         * the normal case, not the exception. */
        const std::string head = text.substr(0, ui->caret);
        float caretX = sn_measure(ui, SN_F_SMALL, head.c_str(), 0.0f);
        float shift = 0;
        const float inner = r.width - pad * 2;
        if (focused && caretX > inner) shift = caretX - inner;

        BeginScissorMode((int)(r.x + 1), (int)r.y, (int)(r.width - 2), (int)r.height);
        sn_text(ui, SN_F_SMALL, text.c_str(), r.x + pad - shift, ty, SN_TEXT);
        if (focused && std::fmod(GetTime(), 1.0) < 0.5)
            DrawRectangle((int)(r.x + pad + caretX - shift), (int)ty, 1, SN_F_SMALL, SN_TEXT);
        EndScissorMode();
    }

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

static bool inside_poly(const Vector2 *p, int n, float x, float y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((p[i].y > y) != (p[j].y > y)) &&
            (x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x))
            in = !in;
    }
    return in;
}

static float dist_to_poly(const Vector2 *p, int n, float x, float y)
{
    float best = 1e30f;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
        const float len2 = dx * dx + dy * dy;
        float t = 0.0f;
        if (len2 > 1e-9f) {
            t = ((x - p[i].x) * dx + (y - p[i].y) * dy) / len2;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
        }
        const float qx = p[i].x + dx * t - x, qy = p[i].y + dy * t - y;
        const float d = std::sqrt(qx * qx + qy * qy);
        if (d < best) best = d;
    }
    return best;
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

Image sn_star_image(int size)
{
    if (size < 4) size = 4;

    Color *px = (Color *)MemAlloc((unsigned)(size * size) * (unsigned)sizeof(Color));

    const Vector2 c = {size * 0.5f, size * 0.5f};
    /* Inset, so the points do not touch the edge of the tile: a taskbar draws
     * icons hard against their neighbours and a mark that fills its square
     * looks bigger than everything beside it. */
    const float r = size * 0.46f;
    const float stroke = r * 0.06f;
    const bool detail = (size >= 28);

    Vector2 pts[10];
    star_points(pts, c, r, 0.0f);

    const float visX0 = c.x - r * STAR_VIS_W * 0.5f, visX1 = c.x + r * STAR_VIS_W * 0.5f;
    const float visY0 = c.y + r * STAR_VIS_Y, visY1 = visY0 + r * STAR_VIS_H;
    const float strX0 = c.x - r * STAR_STR_W * 0.5f, strX1 = c.x + r * STAR_STR_W * 0.5f;
    const float strY0 = c.y + r * STAR_STR_Y, strY1 = strY0 + r * STAR_STR_H;

    enum { SS = 3 };

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int hits = 0, rSum = 0, gSum = 0, bSum = 0;

            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    const float fx = (float)x + ((float)sx + 0.5f) / (float)SS;
                    const float fy = (float)y + ((float)sy + 0.5f) / (float)SS;
                    if (!inside_poly(pts, 10, fx, fy)) continue;

                    Color k;
                    if (fx >= strX0 && fx < strX1 && fy >= strY0 && fy < strY1) k = SN_ALERT;
                    else if (detail && fx >= visX0 && fx < visX1 && fy >= visY0 && fy < visY1)
                        k = SN_VISOR;
                    else if (dist_to_poly(pts, 10, fx, fy) < stroke) k = SN_STAR_EDGE;
                    else k = SN_STAR;

                    hits++;
                    rSum += k.r;
                    gSum += k.g;
                    bSum += k.b;
                }
            }

            Color out = {0, 0, 0, 0};
            if (hits) {
                out.r = (unsigned char)(rSum / hits);
                out.g = (unsigned char)(gSum / hits);
                out.b = (unsigned char)(bSum / hits);
                out.a = (unsigned char)((hits * 255) / (SS * SS));
            }
            px[y * size + x] = out;
        }
    }

    Image img;
    img.data = px;
    img.width = size;
    img.height = size;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return img;
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
