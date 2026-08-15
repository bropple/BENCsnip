/*
 * BENCsnip - one place for the version number.
 *
 * The window title, the --version output, the project file header and the
 * release archive names all read from here, so a release is one edit.
 */

#ifndef SN_VERSION_H
#define SN_VERSION_H

#define SN_VERSION_MAJOR 0
#define SN_VERSION_MINOR 2
#define SN_VERSION_PATCH 0

#define SN_STR2(x) #x
#define SN_STR(x)  SN_STR2(x)

#define SN_VERSION  SN_STR(SN_VERSION_MAJOR) "." SN_STR(SN_VERSION_MINOR) \
                    "." SN_STR(SN_VERSION_PATCH)

#define SN_NAME     "BENCsnip"

#endif /* SN_VERSION_H */
