/*
 * BENCsnip - the platform's own colour picker. See sn_colordlg.h.
 *
 * Windows and Unix are here; macOS is in sn_colordlg_mac.mm, because AppKit is
 * a method call rather than a program to run - the same split sn_filedlg has,
 * and for the same reason.
 */

#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "sn_colordlg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ------------------------------------------------------------------ */
#if defined(_WIN32)

#include <windows.h>

#include <commdlg.h>

int sn_color_dialog(void *owner, const char *title, unsigned *rgba)
{
    (void)title; /* ChooseColor's dialog is titled by the system */
    if (!rgba) return SN_COLOR_CANCELLED;

    /* The sixteen custom slots are static so that a colour mixed for the fill
     * is still there when the outline is picked a moment later. ChooseColor
     * writes into this array, which is the only way it remembers anything. */
    static COLORREF custom[16];
    static bool init;
    if (!init) {
        for (int i = 0; i < 16; i++) custom[i] = RGB(255, 255, 255);
        init = true;
    }

    const unsigned in = *rgba;
    const int r = (int)((in >> 24) & 0xff), g = (int)((in >> 16) & 0xff);
    const int b = (int)((in >> 8) & 0xff);

    CHOOSECOLORA cc;
    std::memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = (HWND)owner;
    cc.lpCustColors = custom;
    cc.rgbResult = RGB(r, g, b);
    /* Open on the mixer rather than on the grid of forty-eight basic colours:
     * this is a video editor and the colour wanted is rarely one of those. */
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;

    if (!ChooseColorA(&cc)) return SN_COLOR_CANCELLED;

    /* Alpha is kept: ChooseColor has no idea there is one. */
    *rgba = ((unsigned)GetRValue(cc.rgbResult) << 24) |
            ((unsigned)GetGValue(cc.rgbResult) << 16) |
            ((unsigned)GetBValue(cc.rgbResult) << 8) | (in & 0xffu);
    return SN_COLOR_OK;
}

/* ------------------------------------------------------------------ */
#elif !defined(__APPLE__)

/* Everything the command printed, trimmed. Empty means it could not be run or
 * the person said no, which for a picker are the same outcome. */
static int read_all(const char *cmd, char *out, size_t cap)
{
    FILE *p = popen(cmd, "r");
    if (!p) return 0;

    size_t n = 0;
    int c;
    while ((c = fgetc(p)) != EOF && n + 1 < cap) out[n++] = (char)c;
    out[n] = 0;
    pclose(p);

    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return n > 0;
}

static int have(const char *tool)
{
    char cmd[64];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    return std::system(cmd) == 0;
}

/* Both of these answer in one of two shapes and neither documents which:
 * "#rrggbb" from kdialog and older zenity, "rgb(r,g,b)" or "rgba(r,g,b,a)"
 * from a zenity built against a newer GTK. Parsed rather than assumed. */
static int parse_color(const char *s, unsigned *rgba)
{
    while (*s == ' ') s++;

    if (*s == '#') {
        unsigned r = 0, g = 0, b = 0;
        /* GTK has been known to answer in sixteen bits per channel:
         * #rrrrggggbbbb. The first two digits of each are the ones wanted. */
        const size_t n = std::strlen(s + 1);
        if (n >= 12) {
            char t[13];
            snprintf(t, sizeof t, "%s", s + 1);
            unsigned rr, gg, bb;
            if (sscanf(t, "%4x%4x%4x", &rr, &gg, &bb) != 3) return 0;
            r = rr >> 8; g = gg >> 8; b = bb >> 8;
        } else if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) != 3) {
            return 0;
        }
        *rgba = (*rgba & 0xffu) | (r << 24) | (g << 16) | (b << 8);
        return 1;
    }

    if (!strncmp(s, "rgb", 3)) {
        int r = 0, g = 0, b = 0;
        double al = -1.0;
        const char *open = std::strchr(s, '(');
        if (!open) return 0;
        const int got = sscanf(open + 1, "%d , %d , %d , %lf", &r, &g, &b, &al);
        if (got < 3) return 0;

        unsigned a = *rgba & 0xffu;
        if (got >= 4 && al >= 0.0 && al <= 1.0) a = (unsigned)(al * 255.0 + 0.5);

        *rgba = ((unsigned)(r & 0xff) << 24) | ((unsigned)(g & 0xff) << 16) |
                ((unsigned)(b & 0xff) << 8) | a;
        return 1;
    }
    return 0;
}

int sn_color_dialog(void *owner, const char *title, unsigned *rgba)
{
    (void)owner; /* X11 parents these itself */
    if (!rgba) return SN_COLOR_CANCELLED;

    char cmd[512], out[256];
    char hex[16];
    snprintf(hex, sizeof hex, "#%02x%02x%02x", (*rgba >> 24) & 0xff,
             (*rgba >> 16) & 0xff, (*rgba >> 8) & 0xff);

    if (have("zenity")) {
        snprintf(cmd, sizeof cmd,
                 "zenity --color-selection --title='%s' --color='%s' 2>/dev/null",
                 title ? title : "Colour", hex);
        if (!read_all(cmd, out, sizeof out)) return SN_COLOR_CANCELLED;
        return parse_color(out, rgba) ? SN_COLOR_OK : SN_COLOR_CANCELLED;
    }
    if (have("kdialog")) {
        snprintf(cmd, sizeof cmd, "kdialog --title '%s' --getcolor 2>/dev/null",
                 title ? title : "Colour");
        if (!read_all(cmd, out, sizeof out)) return SN_COLOR_CANCELLED;
        return parse_color(out, rgba) ? SN_COLOR_OK : SN_COLOR_CANCELLED;
    }
    return SN_COLOR_UNAVAILABLE;
}

#endif /* the macOS case is in sn_colordlg_mac.mm */
