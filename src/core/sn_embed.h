/*
 * BENCsnip - assets compiled into the binary
 *
 * Generated into src/core/sn_embed.c by tools/mkembed.c at build time. The
 * files in assets/ stay the single source; nothing here is committed.
 *
 * The font is in here because a font beside an executable is a font that can
 * go missing - and when it goes missing the window comes up in raylib's
 * fallback face without saying so. The licences are in here because embedding
 * the font without them would not be allowed: the OFL lets the font be bundled
 * with software provided every copy carries the copyright notice and the
 * licence in a form the user can easily view. The information window is that
 * form.
 */

#ifndef SN_EMBED_H
#define SN_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

/* Terminus (TTF), unmodified. */
extern const unsigned char SN_FONT_TTF[];
extern const unsigned int SN_FONT_TTF_LEN;

/* The BENCO wordmark, white on transparent. */
extern const unsigned char SN_LOGO_PNG[];
extern const unsigned int SN_LOGO_PNG_LEN;

/* The program's icon - S. Tarr in front of a film camera - at the sizes a
 * window manager asks for. Four of them rather than one scaled at startup:
 * the 16 and 24 pixel versions have the film strip taken out of them, so they
 * are not the big one made smaller and cannot be produced from it. See
 * tools/make-icons.sh.
 *
 * On Windows the same artwork also arrives as a resource compiled into the
 * executable, which is what the taskbar reads before the program has run a
 * line of its own; these are what it uses afterwards, and on X11 they are the
 * only copy there is. */
extern const unsigned char SN_ICON_16[];
extern const unsigned int SN_ICON_16_LEN;
extern const unsigned char SN_ICON_32[];
extern const unsigned int SN_ICON_32_LEN;
extern const unsigned char SN_ICON_48[];
extern const unsigned int SN_ICON_48_LEN;
extern const unsigned char SN_ICON_64[];
extern const unsigned int SN_ICON_64_LEN;

/* Licence texts, NUL-terminated - the repository's own files, so what the
 * window shows and what the archive ships cannot drift apart. */
extern const unsigned char SN_LICENSE_MIT[];
extern const unsigned int SN_LICENSE_MIT_LEN;

extern const unsigned char SN_LICENSE_OFL[];
extern const unsigned int SN_LICENSE_OFL_LEN;

extern const unsigned char SN_NOTICE[];
extern const unsigned int SN_NOTICE_LEN;

#ifdef __cplusplus
}
#endif

#endif /* SN_EMBED_H */
