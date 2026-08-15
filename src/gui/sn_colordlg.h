/*
 * BENCsnip - the platform's own colour picker
 *
 * The same arrangement as sn_filedlg.h, for the same reason: choosing a colour
 * is a thing every one of these systems already does, better than a widget set
 * this program does not have would do it, and it costs nothing to link.
 *
 *   Windows   ChooseColor from comdlg32, which is already linked for the
 *             file dialogs
 *   macOS     NSColorPanel, in sn_colordlg_mac.mm
 *   Unix      zenity or kdialog, whichever is installed
 *
 * What it replaced was four sliders per colour - red, green, blue and alpha,
 * laid out like the crop row in the layout window. They worked, they were
 * wider than the window they were in, and nobody picks a colour by moving
 * three numbers separately. The hex field beside the button is the other half:
 * a colour somebody already has the code for should be typed, not hunted for
 * in a wheel.
 *
 * Alpha is this file's problem rather than the platform's. ChooseColor has no
 * concept of it and zenity's answer sometimes carries one; NSColorPanel does
 * it properly. So `rgba` goes in with an alpha and comes back with the same
 * one unless the picker actually chose a different one, and the window keeps
 * its own control for it.
 */

#ifndef SN_COLORDLG_H
#define SN_COLORDLG_H

/* Same three answers as sn_filedlg.h, and the same reason for the third: on
 * Unix there may be no picker installed, and the caller does something else
 * about that rather than reporting a cancellation nobody made. */
enum {
    SN_COLOR_CANCELLED = 0,
    SN_COLOR_OK = 1,
    SN_COLOR_UNAVAILABLE = -1
};

/* `rgba` is 0xRRGGBBAA in and out - the order TextStyle uses. Unchanged
 * unless the answer is SN_COLOR_OK. */
int sn_color_dialog(void *owner, const char *title, unsigned *rgba);

#endif /* SN_COLORDLG_H */
