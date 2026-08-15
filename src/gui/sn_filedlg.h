/*
 * BENCsnip - native open and save dialogs
 *
 * raylib has no file dialog, and the alternatives are worse than this file.
 * Bundling one of the single-header dialog libraries would be a third-party
 * dependency taken on over a feature that is a system call on every platform
 * this targets. (There is one such header in the tree - stb_truetype, for
 * drawing text on the picture - and the test it had to pass was that nothing
 * the system ships could do the job. A file dialog does not pass it.) So each
 * platform gets the dialog it already has, and nothing is linked that the
 * system does not already ship:
 *
 *   Windows   GetOpenFileName / GetSaveFileName from comdlg32, part of the OS
 *   macOS     NSOpenPanel and NSSavePanel, in sn_filedlg_mac.mm
 *   Unix      zenity or kdialog, whichever is installed
 *
 * The macOS half is a separate file because it is the one that is not a
 * subprocess. It used to be - osascript, beside zenity, on the grounds that
 * all three are the same idea - and starting an AppleScript interpreter to
 * make one method call cost most of a second before the panel appeared. The
 * note at the top of that file has the rest.
 *
 * The Unix case is the only one that can come up empty, so the return value
 * distinguishes "the user said no" from "there is nothing here to ask with".
 * A caller that cannot tell those apart either loses the file or nags about a
 * cancellation the person meant - and BENCsnip does something with the
 * difference: UNAVAILABLE falls back to the browser drawn in sn_dialog.cpp,
 * so a machine with neither zenity nor kdialog can still open a file.
 *
 * The approach is lifted from BENCmouth and BENCsynth, which worked it out
 * first. What is new here is multiple selection, because importing footage is
 * usually importing a folder of it.
 */

#ifndef SN_FILEDLG_H
#define SN_FILEDLG_H

#include <stddef.h>

enum {
    SN_DLG_CANCELLED = 0,   /* the user dismissed it                      */
    SN_DLG_OK = 1,          /* `out` holds one path, or several, newline
                               separated when `multiple` was asked for    */
    SN_DLG_UNAVAILABLE = -1 /* no dialog on this machine; caller decides   */
};

/* `owner` is the native window handle to parent the dialog to - raylib's
 * GetWindowHandle(), or null. Passed in rather than fetched here so this file
 * needs no raylib. `exts` is a space-separated list without dots, e.g.
 * "mp4 mov mkv". */
int sn_open_dialog(void *owner, const char *title, const char *startDir,
                   const char *filterDesc, const char *exts, int multiple,
                   char *out, size_t cap);

int sn_save_dialog(void *owner, const char *title, const char *defaultName,
                   const char *filterDesc, const char *ext, char *out, size_t cap);

/* Attach to the console of whatever started this program, if there is one, so
 * that printing works. Windows only, and a no-op everywhere else.
 *
 * It lives in this file because this is the one that already includes
 * windows.h - which cannot be included beside raylib.h, the two of them
 * having a Rectangle, a CloseWindow and a ShowCursor each. */
void sn_attach_console(void);

#endif /* SN_FILEDLG_H */
