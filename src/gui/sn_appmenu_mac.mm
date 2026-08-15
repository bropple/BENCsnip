/*
 * BENCsnip - the macOS menu bar.
 *
 * GLFW builds the application menu when it makes the window: the one titled
 * after the program, with About, Services, Hide and Quit in it. What it does
 * not build is File and Edit, and on macOS their absence is conspicuous - the
 * bar is there at the top of the screen either way, and a video editor with
 * nothing in it but Quit reads as a program that has not finished loading.
 *
 * So this inserts two menus after the application menu and before Window,
 * which is where a Mac user looks for them, and every item does nothing but
 * post a number into a queue. See sn_appmenu.h for why it is a queue.
 *
 * --- about the missing shortcuts ---
 *
 * Select All, Delete and Split carry no key equivalent, and that is on
 * purpose. A key equivalent on an NSMenuItem is consumed by AppKit before the
 * event ever reaches GLFW, and this program's text fields are drawn by
 * sn_gui.cpp rather than by AppKit: they read Command-A, Command-C, Command-X,
 * Command-V and Backspace out of raylib's key state themselves. Give the Edit
 * menu Command-A and typing a filename can no longer select the filename;
 * give it Backspace and a filename can no longer be corrected at all.
 *
 * The keys still work - they are handled in main.cpp's keys(), against the
 * timeline when nothing has the caret and against the field when something
 * does, which is the behaviour anyway. What is lost is the reminder printed
 * down the right-hand side of the menu, and that is the cheaper half.
 *
 * Undo and Redo do carry theirs, because no text field here implements undo,
 * so there is nothing for Command-Z to be taken away from.
 */

#include "sn_appmenu.h"

#import <AppKit/AppKit.h>

/* --- the queue ---
 *
 * Written by AppKit on the main thread inside glfwPollEvents, read by the
 * frame that follows on the same thread. One thread, so no lock; a ring
 * rather than a single slot because nothing here guarantees a frame runs
 * between two menu picks, and losing a Save because a Split arrived after it
 * would be the kind of fault nobody reports twice. */
enum { QUEUE = 16 };
static int g_queue[QUEUE];
static int g_head, g_tail;

static void push(int cmd)
{
    const int next = (g_head + 1) % QUEUE;
    if (next == g_tail) return;   /* full: sixteen unread picks is not real */
    g_queue[g_head] = cmd;
    g_head = next;
}

int sn_appmenu_take(void)
{
    if (g_tail == g_head) return SN_CMD_NONE;
    const int cmd = g_queue[g_tail];
    g_tail = (g_tail + 1) % QUEUE;
    return cmd;
}

/* --- what every item points at ---
 *
 * One object and one selector for the whole bar. Which item was picked is its
 * tag, which is the command, so there is no table mapping selectors to
 * meanings and no way for the two to disagree. */
@interface SnMenuTarget : NSObject
- (void)fire:(id)sender;
@end

@implementation SnMenuTarget
- (void)fire:(id)sender
{
    push((int)[(NSMenuItem *)sender tag]);
}
@end

/* Never released. It has to outlive every menu item pointing at it, which is
 * the whole run of the program, and NSMenuItem does not retain its target. */
static SnMenuTarget *g_target;

static void add_item(NSMenu *menu, NSString *title, int cmd, NSString *key,
                     NSEventModifierFlags mods)
{
    NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:title
                                                  action:@selector(fire:)
                                           keyEquivalent:key] autorelease];
    item.target = g_target;
    item.tag = cmd;
    if (key.length) item.keyEquivalentModifierMask = mods;
    [menu addItem:item];
}

/* The common case: Command and one letter. */
static void add_cmd_item(NSMenu *menu, NSString *title, int cmd, NSString *key)
{
    add_item(menu, title, cmd, key, NSEventModifierFlagCommand);
}

/* No shortcut at all - see the note at the top of this file. */
static void add_plain_item(NSMenu *menu, NSString *title, int cmd)
{
    add_item(menu, title, cmd, @"", 0);
}

/* A titled submenu, inserted into the bar at `at`. The bar owns the item and
 * the item owns the submenu, so both are autoreleased here and outlive this
 * function by being retained on the way in. */
static NSMenu *add_menu(NSMenu *bar, NSInteger at, NSString *title)
{
    NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:title
                                                  action:NULL
                                           keyEquivalent:@""] autorelease];
    NSMenu *menu = [[[NSMenu alloc] initWithTitle:title] autorelease];

    /* Everything stays enabled. Automatic enabling asks a responder chain
     * whether each item can be performed, and this program has no responder
     * chain - one object answers every item, so the honest answer would
     * always be yes and the walk would cost a frame's worth of message sends
     * on every menu open. What a command does when it cannot be performed is
     * decided where it is dispatched: undo with nothing to undo returns
     * without saying anything, which is what the toolbar button does too.
     *
     * The visible cost is that Undo is never greyed out. Worth revisiting
     * with validateMenuItem: once there is app state to consult here. */
    menu.autoenablesItems = NO;

    item.submenu = menu;
    [bar insertItem:item atIndex:at];
    return menu;
}

void sn_appmenu_install(void)
{
    static bool done;
    if (done) return;
    done = true;

    @autoreleasepool {
        if (!g_target) g_target = [[SnMenuTarget alloc] init];

        /* GLFW made this when it made the window. If menu bar creation was
         * switched off, there is nothing to insert into and one is made here
         * so that File and Edit exist either way. */
        NSMenu *bar = [NSApp mainMenu];
        if (!bar) {
            bar = [[[NSMenu alloc] init] autorelease];
            [NSApp setMainMenu:bar];
        }

        /* After the application menu and before Window, which is where these
         * two belong and where somebody will look for them. Index 0 only when
         * there is no application menu, which means there is no bar either. */
        const NSInteger at = bar.numberOfItems > 0 ? 1 : 0;

        NSMenu *file = add_menu(bar, at, @"File");
        add_cmd_item(file, @"New Project", SN_CMD_NEW, @"n");
        add_cmd_item(file, @"Open Project…", SN_CMD_OPEN, @"o");
        [file addItem:[NSMenuItem separatorItem]];
        add_cmd_item(file, @"Add Media…", SN_CMD_IMPORT, @"i");
        [file addItem:[NSMenuItem separatorItem]];
        add_cmd_item(file, @"Save", SN_CMD_SAVE, @"s");
        add_item(file, @"Save As…", SN_CMD_SAVE_AS, @"s",
                 NSEventModifierFlagCommand | NSEventModifierFlagShift);
        [file addItem:[NSMenuItem separatorItem]];
        add_cmd_item(file, @"Export…", SN_CMD_EXPORT, @"e");

        NSMenu *edit = add_menu(bar, at + 1, @"Edit");
        add_cmd_item(edit, @"Undo", SN_CMD_UNDO, @"z");
        add_item(edit, @"Redo", SN_CMD_REDO, @"z",
                 NSEventModifierFlagCommand | NSEventModifierFlagShift);
        [edit addItem:[NSMenuItem separatorItem]];
        add_plain_item(edit, @"Split at Playhead", SN_CMD_SPLIT);
        add_plain_item(edit, @"Delete", SN_CMD_DELETE);
        add_plain_item(edit, @"Delete and Close the Gap", SN_CMD_RIPPLE_DELETE);
        [edit addItem:[NSMenuItem separatorItem]];
        add_plain_item(edit, @"Select All", SN_CMD_SELECT_ALL);
        add_plain_item(edit, @"Deselect", SN_CMD_DESELECT);
    }
}
