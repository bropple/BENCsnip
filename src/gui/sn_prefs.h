/*
 * BENCsnip - what the program remembers between runs
 *
 * How the two drawers were left, and nothing else yet.
 *
 * Deliberately not App. This file is included on Windows by a translation unit
 * that reaches for the platform's idea of where settings live, and App drags in
 * raylib, which cannot share a header with windows.h - the two have a
 * Rectangle, a CloseWindow and a ShowCursor each. A plain struct of the things
 * being saved keeps that problem out of here, and keeps this testable without a
 * window.
 *
 * The file is `key value` lines, the same shape as a project file: readable,
 * diffable, and forgiving of a key it has never heard of - which is what makes
 * it safe to add one later without a version number.
 */

#ifndef SN_PREFS_H
#define SN_PREFS_H

#include <string>

namespace sn {

struct Prefs {
    /* The media drawer starts where it has always been: out, and staying.
     * The inspector too, because the thing it describes is the thing on
     * screen, and a panel about the selection is wanted more often than not. */
    bool binOpen = true;
    bool binPinned = true;
    bool inspectOpen = true;
    bool inspectPinned = true;
    int inspectPage = 0;
};

/* Where the file is, for the information window to be able to say. Empty when
 * the platform will not tell us where settings go, in which case nothing is
 * saved and nothing is loaded and the program still works. */
std::string prefsPath();

/* False when there was no file to read, which is the first run and is not an
 * error - `out` is left at its defaults. */
bool prefsLoad(Prefs *out);

/* Best effort. A settings file that cannot be written is not worth telling
 * somebody about in the middle of closing the program. */
void prefsSave(const Prefs &p);

} /* namespace sn */

#endif /* SN_PREFS_H */
