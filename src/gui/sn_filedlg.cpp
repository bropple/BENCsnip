/*
 * BENCsnip - native open and save dialogs.
 * See sn_filedlg.h for why this is three platform cases and not a library.
 */

/* popen/pclose are POSIX, and the C++ standard alone does not declare them.
 * Only the zenity/kdialog case runs a program now - macOS calls its panels
 * directly from sn_filedlg_mac.mm - so this is for that case alone. */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "sn_filedlg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using std::memset;
using std::snprintf;
using std::strlen;

/* ------------------------------------------------------------------ */
#if defined(_WIN32)

/* windows.h first, and not alphabetically. commdlg.h does not include it and
 * cannot stand alone: on its own it pulls in prsht.h, which uses UINT,
 * CALLBACK and LPCDLGTEMPLATE before anything has defined them, and the
 * compiler reports a hundred errors inside the system headers with nothing
 * pointing at the include order that caused them. Sorting these two lines is
 * enough to break the Windows build. */
#include <windows.h>

#include <commdlg.h>

void sn_attach_console(void)
{
    /* Only when standard output has nowhere to go.
     *
     * If it is already a pipe or a file - `bencsnip --timing > out.txt`, or
     * anything that captured this program's output, which is what a CI runner
     * does - then writing works and there is nothing to fix. Attaching a
     * console and pointing stdout at it in that case does the opposite of the
     * intent: it takes the output away from whoever asked for it and puts it
     * on a screen nobody is reading. Which is exactly what the first version
     * of this did, and why a CI run printed nothing at all.
     */
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    FILE *f = nullptr;
    if (freopen_s(&f, "CONOUT$", "w", stdout) != 0) { /* nothing to be done */ }
    if (freopen_s(&f, "CONOUT$", "w", stderr) != 0) { /* nor here */ }
}

/* The filter is a run of NUL-terminated strings ending in a second NUL, which
 * is why it is assembled by hand rather than with one snprintf. */
static void build_filter(char *filter, size_t cap, const char *desc, const char *exts)
{
    /* "mp4 mov mkv" becomes "*.mp4;*.mov;*.mkv". */
    char pattern[512];
    size_t p = 0;
    const char *s = exts;
    while (*s && p + 8 < sizeof pattern) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (p) pattern[p++] = ';';
        pattern[p++] = '*';
        pattern[p++] = '.';
        while (*s && *s != ' ' && p + 1 < sizeof pattern) pattern[p++] = *s++;
    }
    pattern[p] = 0;

    size_t n = 0;
    n += (size_t)snprintf(filter + n, cap - n, "%s", desc);
    filter[n++] = '\0';
    n += (size_t)snprintf(filter + n, cap - n, "%s", pattern);
    filter[n++] = '\0';
    n += (size_t)snprintf(filter + n, cap - n, "All files (*.*)");
    filter[n++] = '\0';
    n += (size_t)snprintf(filter + n, cap - n, "*.*");
    filter[n++] = '\0';
    filter[n++] = '\0';
}

int sn_open_dialog(void *owner, const char *title, const char *startDir,
                   const char *filterDesc, const char *exts, int multiple,
                   char *out, size_t cap)
{
    OPENFILENAMEA ofn;
    static char path[32768];       /* multi-select returns a long list */
    char filter[1024];

    if (!out || cap < 2) return SN_DLG_CANCELLED;

    build_filter(filter, sizeof filter, filterDesc, exts);
    path[0] = '\0';

    std::memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = (HWND)owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)sizeof path;
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = startDir;
    /* NOCHANGEDIR because a file dialog that moves the process's working
     * directory turns every later relative path into a different path. */
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (multiple) ofn.Flags |= OFN_ALLOWMULTISELECT;

    if (!GetOpenFileNameA(&ofn)) return SN_DLG_CANCELLED;

    /* One file comes back as a path. Several come back as a directory, a NUL,
     * then each name, then a second NUL - which has to be reassembled. */
    const char *p = path;
    const size_t dirLen = strlen(p);
    if (p[dirLen + 1] == '\0') {
        snprintf(out, cap, "%s", path);
        return SN_DLG_OK;
    }

    size_t n = 0;
    const char *dir = p;
    p += dirLen + 1;
    while (*p && n + 2 < cap) {
        n += (size_t)snprintf(out + n, cap - n, "%s%s\\%s", n ? "\n" : "", dir, p);
        p += strlen(p) + 1;
    }
    return SN_DLG_OK;
}

int sn_save_dialog(void *owner, const char *title, const char *defaultName,
                   const char *filterDesc, const char *ext, char *out, size_t cap)
{
    OPENFILENAMEA ofn;
    char path[1024];
    char filter[512];

    if (!out || cap < 2) return SN_DLG_CANCELLED;

    build_filter(filter, sizeof filter, filterDesc, ext);
    snprintf(path, sizeof path, "%s", defaultName ? defaultName : "");

    std::memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = (HWND)owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)sizeof path;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;

    if (!GetSaveFileNameA(&ofn)) return SN_DLG_CANCELLED;

    snprintf(out, cap, "%s", path);
    return SN_DLG_OK;
}

/* ------------------------------------------------------------------ */
#else

/* Nothing to do: a terminal-launched program on these systems already has
 * standard output going somewhere. */
void sn_attach_console(void) {}

/* ------------------------------------------------------------------ */
#if !defined(__APPLE__)

/* Everything the command printed, up to `cap`. Returns 0 when the command
 * could not be run or printed nothing - which for these dialogs means
 * cancelled. */
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

/* "mp4 mov mkv" -> "*.mp4 *.mov *.mkv", which is the shape both zenity's
 * --file-filter and kdialog's filter argument want. */
static void patterns(const char *exts, char *out, size_t cap, char sep)
{
    size_t n = 0;
    const char *s = exts;
    while (*s && n + 8 < cap) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (n) out[n++] = sep;
        out[n++] = '*';
        out[n++] = '.';
        while (*s && *s != ' ' && n + 1 < cap) out[n++] = *s++;
    }
    out[n] = 0;
}

int sn_open_dialog(void *owner, const char *title, const char *startDir,
                   const char *filterDesc, const char *exts, int multiple,
                   char *out, size_t cap)
{
    char cmd[2048], pat[512];

    (void)owner; /* X11 parents these dialogs itself */
    if (!out || cap < 2) return SN_DLG_CANCELLED;
    if (!startDir) startDir = ".";

    if (have("zenity")) {
        patterns(exts, pat, sizeof pat, ' ');
        snprintf(cmd, sizeof cmd,
                 "zenity --file-selection %s --separator='\n' --title='%s' "
                 "--filename='%s/' --file-filter='%s | %s' "
                 "--file-filter='All files | *' 2>/dev/null",
                 multiple ? "--multiple" : "", title, startDir, filterDesc, pat);
        return read_all(cmd, out, cap) ? SN_DLG_OK : SN_DLG_CANCELLED;
    }
    if (have("kdialog")) {
        patterns(exts, pat, sizeof pat, ' ');
        snprintf(cmd, sizeof cmd,
                 "kdialog --title '%s' %s --getopenfilename '%s' '%s|%s' 2>/dev/null",
                 title, multiple ? "--multiple --separate-output" : "", startDir, pat,
                 filterDesc);
        return read_all(cmd, out, cap) ? SN_DLG_OK : SN_DLG_CANCELLED;
    }
    return SN_DLG_UNAVAILABLE;
}

int sn_save_dialog(void *owner, const char *title, const char *defaultName,
                   const char *filterDesc, const char *ext, char *out, size_t cap)
{
    char cmd[2048];

    (void)owner;
    if (!out || cap < 2) return SN_DLG_CANCELLED;
    if (!defaultName) defaultName = "";

    if (have("zenity")) {
        snprintf(cmd, sizeof cmd,
                 "zenity --file-selection --save --confirm-overwrite --title='%s' "
                 "--filename='%s' --file-filter='%s | *.%s' "
                 "--file-filter='All files | *' 2>/dev/null",
                 title, defaultName, filterDesc, ext);
        return read_all(cmd, out, cap) ? SN_DLG_OK : SN_DLG_CANCELLED;
    }
    if (have("kdialog")) {
        snprintf(cmd, sizeof cmd,
                 "kdialog --title '%s' --getsavefilename '%s' '*.%s|%s' 2>/dev/null",
                 title, defaultName, ext, filterDesc);
        return read_all(cmd, out, cap) ? SN_DLG_OK : SN_DLG_CANCELLED;
    }
    return SN_DLG_UNAVAILABLE;
}

#endif /* !__APPLE__ - the panels are in sn_filedlg_mac.mm */

#endif /* !_WIN32 */
