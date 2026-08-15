#!/bin/sh
# Put a clock inside GLFW's window creation.
#
# For one machine where every raylib program takes ten and a half seconds to
# open a window, all of it inside glfwCreateWindow. Every Windows call that
# GLFW makes has been timed by hand in tools/glprobe.c and they are all fast -
# the whole sequence comes back in 317 ms there - so the time is going
# somewhere between them that only the real code can show.
#
# raylib is built from source here, so this edits the copy in a build tree
# before it is compiled: a timestamp printed at each step of window creation,
# to stderr, so the last line before the gap names what is slow.
#
# Not part of any release. Run against a raylib checkout, build it, build
# BENCsnip against it, and read the output. See .github/workflows/instrumented.yml.
#
#   tools/instrument-glfw.sh /tmp/raylib
#
set -eu

RL="${1:?usage: instrument-glfw.sh <raylib source dir>}"
SRC="$RL/src"
GLFW="$SRC/external/glfw/src"

test -f "$SRC/rglfw.c" || { echo "not a raylib source tree: $RL" >&2; exit 1; }

# Insert a line before the first line containing an anchor. Fails loudly when
# the anchor is missing, because a patch that silently does nothing is worse
# than no patch: the build would succeed and print nothing, and the missing
# output would be read as a result.
ins() {
    file="$1"; anchor="$2"; text="$3"
    grep -qF -- "$anchor" "$file" || return 1
    awk -v a="$anchor" -v t="$text" '
        !done && index($0, a) { print t; done = 1 }
        { print }
    ' "$file" > "$file.tmp"
    mv "$file.tmp" "$file"
}

# The same, but an anchor that has moved is a hard error. A patch that
# silently does nothing is worse than no patch: the build succeeds, prints
# nothing, and the missing output gets read as a result.
must() {
    ins "$@" || {
        echo "instrument-glfw: anchor not found in $(basename "$1"): $2" >&2
        exit 1
    }
}

# Put a line either side of one exact line, for a call that needs bracketing
# rather than announcing. Used where the line after the call is not unique in
# the file and cannot be an anchor of its own.
wrap() {
    file="$1"; anchor="$2"; before="$3"; after="$4"
    grep -qF -- "$anchor" "$file" || {
        echo "instrument-glfw: anchor not found in $(basename "$file"): $anchor" >&2
        exit 1
    }
    awk -v a="$anchor" -v b="$before" -v c="$after" '
        !done && index($0, a) { print b; print $0; print c; done = 1; next }
        { print }
    ' "$file" > "$file.tmp"
    mv "$file.tmp" "$file"
}

HELPER='/* --- added by BENCsnip tools/instrument-glfw.sh --- */
#if defined(_WIN32)
#include <stdio.h>
#include <time.h>
static double sn_probe_ms(void)
{
    static clock_t s;
    if (!s) s = clock();
    return (double)(clock() - s) * 1000.0 / (double)CLOCKS_PER_SEC;
}
/* clock() on Windows is wall time since the process started, which is what is
 * wanted here: the thing being chased is a wait, not work, and a processor
 * clock would show nothing at all. */
#define SNT(x) do { fprintf(stderr, "  [glfw] %9.1f ms  %s\\n", sn_probe_ms(), (x)); fflush(stderr); } while (0)
#else
#define SNT(x) do { } while (0)
#endif
/* --- end --- */'

# One translation unit: rglfw.c includes every GLFW source, so the helper goes
# in once and both files below can use it.
must "$SRC/rglfw.c" '#include "external/glfw/src/init.c"' "$HELPER"

# --- the window itself ------------------------------------------------------
W="$GLFW/win32_window.c"
must "$W" '    if (!createNativeWindow(window, wndconfig, fbconfig))' \
    '    SNT("_glfwCreateWindowWin32 enter");'
must "$W" '    if (ctxconfig->client != GLFW_NO_API)' \
    '    SNT("createNativeWindow done");'
must "$W" '            if (!_glfwInitWGL())' \
    '            SNT("about to _glfwInitWGL");'
must "$W" '            if (!_glfwCreateContextWGL(window, ctxconfig, fbconfig))' \
    '            SNT("_glfwInitWGL done");'
must "$W" '        if (!_glfwRefreshContextAttribs(window, ctxconfig))' \
    '        SNT("_glfwCreateContextWGL done");'
must "$W" '    if (wndconfig->mousePassthrough)' \
    '    SNT("_glfwRefreshContextAttribs done");'

# Inside createNativeWindow, either side of the two calls most likely to sit
# on something: making the window, and handing it to the shell.
must "$W" '    window->win32.handle = CreateWindowExW(exStyle,' \
    '    SNT("about to CreateWindowExW");'
must "$W" '    DragAcceptFiles(window->win32.handle, TRUE);' \
    '    SNT("about to DragAcceptFiles");'

# --- the context ------------------------------------------------------------
C="$GLFW/wgl_context.c"
must "$C" '    nativeCount = DescribePixelFormat(window->context.wgl.dc,' \
    '    SNT("choosePixelFormatWGL enter");'
must "$C" '    closest = _glfwChooseFBConfig(fbconfig, usableConfigs, usableCount);' \
    '    SNT("read every pixel format");'
must "$C" '    pixelFormat = choosePixelFormatWGL(window, ctxconfig, fbconfig);' \
    '    SNT("_glfwCreateContextWGL enter");'

# --- and what raylib does around it ----------------------------------------
R="$SRC/platforms/rcore_desktop_glfw.c"
must "$R" '#include "GLFW/glfw3.h"' "$HELPER"

# The one raylib makes on purpose, and warns about in its own comment:
# GLFW 3.4 defers joystick setup until something asks for a joystick, and
# raylib asks here so that the delay lands before the window rather than on
# the first frame. On Win32 that runs DirectInput8Create and enumerates every
# game controller the machine has ever seen, present or not.
wrap "$R" '    glfwSetJoystickCallback(NULL);' \
    '    SNT("raylib: about to force joystick init (DirectInput enumeration)");' \
    '    SNT("raylib: joystick init done");'

must "$R" '        platform.handle = glfwCreateWindow(creationWidth, creationHeight,' \
    '        SNT("raylib: about to glfwCreateWindow");'
must "$R" '    glfwMakeContextCurrent(platform.handle);' \
    '    SNT("raylib: glfwCreateWindow and monitor queries done");'
must "$R" '        glfwSwapInterval(0);        // No V-Sync by default' \
    '        SNT("raylib: glfwMakeContextCurrent done");'

echo "instrument-glfw: patched $RL"
