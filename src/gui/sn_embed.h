/*
 * BENCsnip GUI - assets compiled into the binary
 *
 * Generated into src/gui/sn_embed.c by tools/mkembed.c at build time. The
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

/* S. Tarr, for platforms where an executable has no icon resource. */
extern const unsigned char SN_ICON_PNG[];
extern const unsigned int SN_ICON_PNG_LEN;

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
