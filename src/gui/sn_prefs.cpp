/*
 * BENCsnip - what the program remembers between runs. See sn_prefs.h.
 */

#include "sn_prefs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace sn {

namespace {

const char SEP =
#if defined(_WIN32)
    '\\';
#else
    '/';
#endif

/* Where this platform keeps a program's settings.
 *
 * Each of the three has one answer and they are not the same answer, so this
 * is three cases rather than a dot-file in the home directory - which is the
 * wrong place on two of them and an eyesore on the third. */
std::string config_dir()
{
#if defined(_WIN32)
    if (const char *base = getenv("APPDATA"))
        if (*base) return std::string(base) + "\\BENCO\\BENCsnip";
#elif defined(__APPLE__)
    if (const char *home = getenv("HOME"))
        if (*home) return std::string(home) + "/Library/Application Support/BENCsnip";
#else
    if (const char *xdg = getenv("XDG_CONFIG_HOME"))
        if (*xdg) return std::string(xdg) + "/bencsnip";
    if (const char *home = getenv("HOME"))
        if (*home) return std::string(home) + "/.config/bencsnip";
#endif
    return std::string();
}

/* Every directory on the way, ignoring the ones already there. */
bool make_dirs(const std::string &path)
{
    if (path.empty()) return false;

    for (size_t i = 1; i <= path.size(); i++) {
        if (i < path.size() && path[i] != SEP) continue;

        const std::string part = path.substr(0, i);
        if (part.empty()) continue;
#if defined(_WIN32)
        /* "C:" is a drive rather than a directory, and asking to make one is
         * an error that does not matter. */
        if (part.size() == 2 && part[1] == ':') continue;
        _mkdir(part.c_str());
#else
        mkdir(part.c_str(), 0755);
#endif
    }
    return true;
}

bool truthy(const char *s)
{
    while (*s == ' ') s++;
    return *s == '1' || *s == 't' || *s == 'y';
}

} /* namespace */

std::string prefsPath()
{
    const std::string dir = config_dir();
    if (dir.empty()) return std::string();
    return dir + SEP + "settings";
}

bool prefsLoad(Prefs *out)
{
    if (!out) return false;

    const std::string path = prefsPath();
    if (path.empty()) return false;

    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (line[0] == '#' || line[0] == 0) continue;

        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char *key = line, *val = sp + 1;

        /* A key this build has never heard of is skipped rather than
         * complained about: that is what lets a newer version add one and an
         * older one still open the file. */
        if (!strcmp(key, "bin.open")) out->binOpen = truthy(val);
        else if (!strcmp(key, "bin.pinned")) out->binPinned = truthy(val);
        else if (!strcmp(key, "inspect.open")) out->inspectOpen = truthy(val);
        else if (!strcmp(key, "inspect.pinned")) out->inspectPinned = truthy(val);
        else if (!strcmp(key, "inspect.page")) out->inspectPage = atoi(val) ? 1 : 0;
    }
    fclose(f);
    return true;
}

void prefsSave(const Prefs &p)
{
    const std::string dir = config_dir();
    if (dir.empty()) return;
    make_dirs(dir);

    const std::string path = prefsPath();
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;

    fprintf(f, "# BENCsnip settings. Delete this file to start over.\n");
    fprintf(f, "bin.open %d\n", p.binOpen ? 1 : 0);
    fprintf(f, "bin.pinned %d\n", p.binPinned ? 1 : 0);
    fprintf(f, "inspect.open %d\n", p.inspectOpen ? 1 : 0);
    fprintf(f, "inspect.pinned %d\n", p.inspectPinned ? 1 : 0);
    fprintf(f, "inspect.page %d\n", p.inspectPage);
    fclose(f);
}

} /* namespace sn */
