#!/bin/sh
#
# Regenerate assets/icon from the star geometry in the program.
#
# The PNGs are written by the editor itself - `bencsnip --icons` calls the same
# sn_star_image() that produces the window icon, which is the same geometry
# sn_star() draws in the information window. There is no separate icon artwork
# to keep in step, and no way for the icon in the taskbar to disagree with the
# icon in the .ico.
#
# The .ico is assembled here because packing several sizes into one container
# is the one step raylib cannot do.
#
# Committed rather than built: a release job should not need ImageMagick, and
# these change roughly never.
#
#   make icons          the usual way in

set -eu

BIN="${BIN:-./bencsnip}"
OUT=assets/icon

if [ ! -x "$BIN" ]; then
    echo "no $BIN - run make first" >&2
    exit 1
fi

mkdir -p "$OUT"
"$BIN" --icons "$OUT" >/dev/null

# ImageMagick 7 calls it `magick`; 6 calls it `convert`. Both are still out
# there, and `convert` is also the name of a Windows system utility that turns
# a FAT volume into NTFS - so a `convert` has to identify itself before it is
# believed.
IM=
if command -v magick >/dev/null 2>&1; then
    IM=magick
elif command -v convert >/dev/null 2>&1 && \
     convert -version 2>/dev/null | grep -qi imagemagick; then
    IM=convert
fi

if [ -z "$IM" ]; then
    echo "warning: no ImageMagick - the PNGs are updated, $OUT/bencsnip.ico is not" >&2
    exit 0
fi

# 16, 24, 32 and 48 are the sizes Windows asks for: the first two for the
# titlebar, the others for the taskbar. 128 and 256 are for Explorer's larger
# views. Anything absent gets scaled from a neighbour, badly.
$IM "$OUT/star-16.png" "$OUT/star-24.png" "$OUT/star-32.png" \
    "$OUT/star-48.png" "$OUT/star-64.png" "$OUT/star-128.png" \
    "$OUT/star-256.png" "$OUT/bencsnip.ico"

echo "wrote $OUT/star-*.png and $OUT/bencsnip.ico"
