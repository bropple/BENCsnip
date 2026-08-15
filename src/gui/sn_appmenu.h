/*
 * BENCsnip - the application menu bar
 *
 * macOS puts a menu bar at the top of the screen whether a program asks for
 * one or not, and a program that does not fill it looks broken in a way no
 * amount of toolbar makes up for. GLFW already creates the application menu -
 * the one with About, Hide and Quit in it - so what is missing is File and
 * Edit, and that is what this adds.
 *
 * Nothing is added to the other platforms yet. A menu bar there is one this
 * program would have to draw itself, inside its own window, because raylib has
 * no widgets and there is no system bar to hang anything on; on Windows it
 * would also want to stay hidden until Alt asks for it. That is a different
 * piece of work with a different interface, and this header is the half of it
 * that both would share: a list of what the commands are, and a queue of which
 * one somebody just picked.
 *
 * Not to be confused with sn_menu_open in sn_gui.h, which is the little popup
 * a right-click puts under the pointer. This one is the bar.
 */

#ifndef SN_APPMENU_H
#define SN_APPMENU_H

/* Every command a menu can carry. The numbers are not written down anywhere
 * outside this program - they go into an NSMenuItem's tag and come back out -
 * so they may be renumbered freely, but keep SN_CMD_NONE at zero: it is what
 * an empty queue returns. */
enum SnCmd {
    SN_CMD_NONE = 0,

    SN_CMD_NEW,
    SN_CMD_OPEN,
    SN_CMD_SAVE,
    SN_CMD_SAVE_AS,
    SN_CMD_IMPORT,
    SN_CMD_EXPORT,

    SN_CMD_UNDO,
    SN_CMD_REDO,
    SN_CMD_ADD_TEXT,
    SN_CMD_SPLIT,
    SN_CMD_DELETE,
    SN_CMD_RIPPLE_DELETE,
    SN_CMD_SELECT_ALL,
    SN_CMD_DESELECT
};

/* Put the menus up. Call it once, after the window exists - on macOS the bar
 * GLFW made is what this inserts into, and before InitWindow there is no
 * NSApplication to have made one. Calling it twice does nothing the second
 * time. A no-op on every platform but macOS. */
void sn_appmenu_install(void);

/* The next command somebody picked, or SN_CMD_NONE when there is none.
 *
 * A queue rather than a callback because a menu action arrives from AppKit in
 * the middle of GLFW's event poll, which is the middle of a frame, and running
 * an editing command from there would mutate the project underneath whatever
 * is currently drawing it. Drain it once a frame, at a point of your choosing.
 * Always SN_CMD_NONE where there are no menus. */
int sn_appmenu_take(void);

#endif /* SN_APPMENU_H */
