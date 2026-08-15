/*
 * BENCsnip - the macOS open and save panels.
 *
 * These used to be an `osascript -e 'choose file ...'` in sn_filedlg.cpp,
 * beside the zenity and kdialog cases, on the grounds that all three are the
 * same idea: ask the system, read back a path. They are not quite. zenity is
 * a program; NSOpenPanel is a class in a framework this process has already
 * loaded, and osascript was a way of reaching it that started a second process
 * and an AppleScript interpreter to make one method call. That cost lands
 * entirely before the panel appears, which is the worst place for it - the
 * window is frozen and there is nothing on screen yet to explain why.
 *
 * Going through the interpreter also meant the request was a string, so every
 * argument had to survive being pasted into a script. It did not: the type
 * list was built with a doubled quote and osascript refused to parse it, which
 * is why both of the buttons that open files did nothing at all for a while,
 * silently. A file whose name contains a quote would have done the same thing.
 * None of that can happen to a method call.
 *
 * The panels are run modally, which is what the synchronous shape of
 * sn_filedlg.h asks for and what the osascript version did too.
 */

#include "sn_filedlg.h"

#import <AppKit/AppKit.h>

/* The C headers rather than their <c...> spellings: snprintf is used
 * unqualified below, and realpath and getenv are POSIX additions that
 * <cstdlib> is not required to put anywhere but std::. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* Null, empty and not-UTF-8 all come back nil rather than throwing, because
 * every one of these strings arrives from somewhere this file does not
 * control and a file dialog is not worth a crash. */
static NSString *str(const char *s)
{
    return (s && *s) ? [NSString stringWithUTF8String:s] : nil;
}

/* The extensions the panel will let through. `exts` is space separated
 * without dots - "mp4 mov mkv" - which is the shape sn_filedlg.h documents.
 *
 * allowedFileTypes has been deprecated since macOS 12 in favour of
 * allowedContentTypes, which takes UTTypes. It is used here anyway: the
 * replacement needs UniformTypeIdentifiers.framework linked and a runtime
 * check for the machines that predate it, and all of that to express
 * "bencsnip" - an extension with no registered type, which UTType would have
 * to invent a dynamic identifier for. The deprecated property still works on
 * every macOS that exists, and takes exactly the strings already in hand.
 */
static void set_types(NSSavePanel *p, const char *exts)
{
    if (!exts || !*exts) return;

    NSMutableArray<NSString *> *list = [NSMutableArray array];
    for (NSString *one in [str(exts) componentsSeparatedByString:@" "])
        if (one.length) [list addObject:one];

    if (!list.count) return;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    p.allowedFileTypes = list;
#pragma clang diagnostic pop
}

/* Where the panel opens.
 *
 * Every caller passes ".", and an app launched from the Finder starts in "/",
 * which is not a useful place to put somebody looking for their footage.
 * Their home folder is. This is the rule the AppleScript version arrived at
 * and there is no reason for it to change. */
static void set_location(NSSavePanel *p, const char *startDir)
{
    char here[PATH_MAX];
    const char *dir = (startDir && realpath(startDir, here)) ? here : nullptr;

    if (!dir || (dir[0] == '/' && dir[1] == 0)) {
        const char *home = getenv("HOME");
        dir = (home && *home) ? home : nullptr;
    }
    if (!dir) return;

    NSString *s = str(dir);
    if (s) p.directoryURL = [NSURL fileURLWithPath:s isDirectory:YES];
}

/* A modal panel nobody can see looks exactly like a program that has hung, so
 * make sure there is a frontmost application to put it in front of.
 *
 * This mattered more when the panel belonged to osascript, which was a process
 * with no presence on screen; here it belongs to this one, and this one is
 * frontmost because somebody just clicked a button in it. It stays because the
 * cost is a message send and the failure it prevents is a window that has to
 * be force-quit.
 *
 * activateIgnoringOtherApps: is discouraged from macOS 14 in favour of plain
 * -activate, on the grounds that an app should not steal focus. This one is
 * not stealing anything - it already has focus - so the older call is the same
 * no-op on every version and does not need a runtime check to make it. */
static void come_forward(void)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
}

int sn_open_dialog(void *owner, const char *title, const char *startDir,
                   const char *filterDesc, const char *exts, int multiple,
                   char *out, size_t cap)
{
    (void)owner;      /* the panel parents itself to the frontmost window */
    (void)filterDesc; /* the panel names the filter from the types alone  */

    if (!out || cap < 2) return SN_DLG_CANCELLED;
    out[0] = '\0';

    @autoreleasepool {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseFiles = YES;
        p.canChooseDirectories = NO;
        p.allowsMultipleSelection = multiple ? YES : NO;
        p.resolvesAliases = YES;

        /* The title bar of an open panel is the system's on modern macOS.
         * `message` is the line that is actually shown. */
        NSString *t = str(title);
        if (t) p.message = t;

        set_types(p, exts);
        set_location(p, startDir);

        come_forward();
        if ([p runModal] != NSModalResponseOK) return SN_DLG_CANCELLED;

        /* Several come back newline separated, which is what sn_filedlg.h
         * promises and what open_files in main.cpp splits on. */
        size_t n = 0;
        for (NSURL *u in p.URLs) {
            const char *s = u.path.fileSystemRepresentation;
            if (!s || !*s) continue;

            const int w = snprintf(out + n, cap - n, "%s%s", n ? "\n" : "", s);
            if (w < 0 || (size_t)w >= cap - n) { out[n] = '\0'; break; }
            n += (size_t)w;
        }
        return n ? SN_DLG_OK : SN_DLG_CANCELLED;
    }
}

int sn_save_dialog(void *owner, const char *title, const char *defaultName,
                   const char *filterDesc, const char *ext, char *out, size_t cap)
{
    (void)owner;
    (void)filterDesc;

    if (!out || cap < 2) return SN_DLG_CANCELLED;
    out[0] = '\0';

    @autoreleasepool {
        NSSavePanel *p = [NSSavePanel savePanel];
        p.canCreateDirectories = YES;

        NSString *t = str(title);
        if (t) p.message = t;

        NSString *name = str(defaultName);
        if (name) p.nameFieldStringValue = name;

        set_types(p, ext);
        /* No directoryURL on purpose. There is no start directory in this
         * function's signature to honour, and left alone a save panel opens
         * where this app last saved something - which is a better guess than
         * anything that could be invented here, and is what the osascript
         * version did by having no `default location` either. */

        come_forward();
        if ([p runModal] != NSModalResponseOK) return SN_DLG_CANCELLED;

        const char *s = p.URL.path.fileSystemRepresentation;
        if (!s || !*s) return SN_DLG_CANCELLED;

        snprintf(out, cap, "%s", s);
        return SN_DLG_OK;
    }
}
