/*
 * BENCsnip - the application menu bar, everywhere it does not exist yet.
 *
 * See sn_appmenu.h. macOS is in sn_appmenu_mac.mm; this is Windows and Unix,
 * where there is no system menu bar to fill and the drawn one has not been
 * written. The stubs are here rather than behind an #ifdef at every call site
 * so that main.cpp asks for the menus unconditionally and the answer is a
 * function that returns nothing to do.
 */

#include "sn_appmenu.h"

#if !defined(__APPLE__)

void sn_appmenu_install(void) {}

int sn_appmenu_take(void) { return SN_CMD_NONE; }

#endif
