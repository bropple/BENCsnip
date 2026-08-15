/*
 * BENCsnip - the macOS colour picker. See sn_colordlg.h.
 *
 * NSColorPanel is not a dialog with an OK and a Cancel in it: it is a tool
 * that sits there and applies as you move, and it has only a close button.
 * So "the answer" is the colour it is showing when it goes away, and a person
 * who closes it without touching anything gets back what they came in with -
 * which is a cancellation by any other name, and needs no separate button to
 * say so.
 *
 * The wait is a modal session driven by hand rather than -runModalForWindow:.
 * That call ends when something calls -stopModal, and nothing does when a
 * panel is closed by its own close button; the loop below ends when the panel
 * is no longer on screen, which is the thing actually being waited for.
 */

#include "sn_colordlg.h"

#import <AppKit/AppKit.h>

int sn_color_dialog(void *owner, const char *title, unsigned *rgba)
{
    (void)owner;
    if (!rgba) return SN_COLOR_CANCELLED;

    @autoreleasepool {
        const unsigned in = *rgba;

        NSColorPanel *panel = [NSColorPanel sharedColorPanel];
        panel.showsAlpha = YES;      /* the one platform here that can */
        if (title) panel.title = [NSString stringWithUTF8String:title];

        [panel setColor:[NSColor colorWithSRGBRed:((in >> 24) & 0xff) / 255.0
                                            green:((in >> 16) & 0xff) / 255.0
                                             blue:((in >> 8) & 0xff) / 255.0
                                            alpha:(in & 0xff) / 255.0]];

        [NSApp activateIgnoringOtherApps:YES];
        [panel makeKeyAndOrderFront:nil];

        NSModalSession session = [NSApp beginModalSessionForWindow:panel];
        while ([NSApp runModalSession:session] == NSModalResponseContinue) {
            if (!panel.isVisible) break;
        }
        [NSApp endModalSession:session];

        /* sRGB, because that is the space the numbers in a project file mean.
         * A colour dragged in from somewhere else can be in a space with no
         * red, green and blue components to read at all, and asking for them
         * without converting first throws. */
        NSColor *c = [panel.color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        if (!c) return SN_COLOR_CANCELLED;

        CGFloat r = 0, g = 0, b = 0, a = 1;
        [c getRed:&r green:&g blue:&b alpha:&a];

        auto byte = [](CGFloat v) {
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            return (unsigned)(v * 255.0 + 0.5);
        };

        *rgba = (byte(r) << 24) | (byte(g) << 16) | (byte(b) << 8) | byte(a);
        return SN_COLOR_OK;
    }
}
