/*
 * BENCsnip - the project file
 *
 * A .bencsnip is lines of text. Not because a binary format would be hard,
 * but because a project file is the one thing a user might have to look at
 * when something has gone wrong - a moved drive, a renamed folder - and a
 * file you can open in a text editor and fix is worth more than one you
 * cannot.
 *
 * It holds no media. A clip is a path, an in point and an out point; the
 * files stay where they are.
 */

#ifndef SN_PROJECT_H
#define SN_PROJECT_H

#include "sn_timeline.h"

#include <string>

namespace sn {

bool saveProject(const Project &p, const std::string &path, std::string *err);

/* Re-probes every file as it loads, so a project opened after a clip was
 * re-encoded or replaced picks up the new duration rather than trusting what
 * was true last week. A file that has gone missing is kept in the bin, marked
 * missing, with the duration the project remembered - the edit survives, and
 * relinking one path brings it back. */
bool loadProject(Project *p, const std::string &path, std::string *err);

/* Point a bin item at a different file. Returns false if the new file cannot
 * be probed. */
bool relink(Project &p, int itemId, const std::string &path, std::string *err);

/* Add a file to the bin, or return the id of the one already there. Returns 0
 * and fills err when the file is not media. */
int importFile(Project &p, const std::string &path, std::string *err);

} /* namespace sn */

#endif /* SN_PROJECT_H */
