#!/bin/sh
#
# The two bitmaps the Windows installer draws, generated from the same brand
# assets and the same palette as the macOS disk image.
#
#   tools/make-installer-art.sh
#
# Output is committed, not built at release time: the release job runs on a
# runner that would need ImageMagick installed and the result checked by
# nobody, and art that regenerates on every release is art that can silently
# change. Run this when the brand assets or the palette change, look at what
# came out, and commit it.
#
# Both files are 24-bit BMP because that is what NSIS reads. A PNG is silently
# refused and a 32-bit BMP renders with a black box where the alpha was, which
# is how you ship an installer with a hole in it.
#
# Sizes are fixed by the Modern UI 2 layout, not chosen:
#
#   nsis-welcome.bmp   164 x 314   the panel down the left of the first and
#                                  last pages
#   nsis-header.bmp    150 x  57   the tile at the top right of every page in
#                                  between, and of both uninstaller pages
#
# The header sits on a white strip that MUI draws itself and whose text is
# black, so this is a dark tile on white by design rather than by accident.
# MUI_BGCOLOR would paint that strip too - and leave the black header text on
# top of it, unreadable. See tools/windows-installer.nsi.

set -eu

cd "$(dirname "$0")/.."

MAGICK="${MAGICK:-magick}"
command -v "$MAGICK" >/dev/null 2>&1 || {
    echo "need ImageMagick ($MAGICK not found)" >&2
    exit 1
}

FONT=assets/fonts/TerminusTTF.ttf
MARK=assets/brand/BENCO_Logo_Terminal.png    # white on transparent
STAR=assets/icon/star-256.png
OUT=assets/brand

# src/gui/sn_gui.cpp. The disk image uses these too.
BG='#0c1408'
BORDER='#2a3a1e'
DIM='#8aa878'
ACCENT='#78b946'

# ---------------------------------------------------------------- welcome
#
# Reads top to bottom: who made it, what it is, a rule, S. Tarr, the licence.
# The same order as the disk image window, turned on its side.

"$MAGICK" -size 164x314 "xc:$BG" \
    \( "$MARK" -trim +repage -resize 120x \) -gravity none -geometry +22+34 -composite \
    -font "$FONT" -pointsize 10 -fill "$DIM" \
    -annotate +36+80 'a video editor' \
    -fill "$BORDER" -draw 'rectangle 22,100 142,100' \
    \( "$STAR" -resize 76x76 \) -geometry +44+140 -composite \
    -fill "$DIM" -pointsize 9 \
    -annotate +22+272 'BENCO Holdings' \
    -annotate +22+286 'MIT licensed' \
    -alpha remove -alpha off -type TrueColor -depth 8 \
    "BMP3:$OUT/nsis-welcome.bmp"

# ---------------------------------------------------------------- header
#
# The wordmark and nothing else - it is 150 px wide and repeats on every page,
# so anything more becomes noise by the third one. The accent rule along the
# bottom is the only thing separating it from the white strip it sits on.

"$MAGICK" -size 150x57 "xc:$BG" \
    \( "$MARK" -trim +repage -resize 104x \) -gravity none -geometry +23+16 -composite \
    -fill "$ACCENT" -draw 'rectangle 0,55 149,56' \
    -alpha remove -alpha off -type TrueColor -depth 8 \
    "BMP3:$OUT/nsis-header.bmp"

for f in "$OUT/nsis-welcome.bmp" "$OUT/nsis-header.bmp"; do
    printf '%s  ' "$f"
    "$MAGICK" identify -format '%wx%h %[bit-depth]-bit %m\n' "$f"
done
