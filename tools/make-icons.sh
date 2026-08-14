#!/bin/sh
#
# The program's icon, in every size and format something asks for it in.
#
#   tools/make-icons.sh          the usual way in is `make icons`
#
# The source is assets/icon/film_camera_star.svg: S. Tarr in front of an old
# film camera, on a strip of film. Everything here comes out of that one file,
# so there is no second copy of the artwork to keep in step and no way for the
# taskbar icon to disagree with the one in the installer or the one inside the
# executable.
#
# Two things happen to it on the way out, both in tools/icon-variant.py:
#
#   The background rect goes, always. An icon sits on a dark taskbar, a light
#   desktop and the macOS Dock, and a flat plate behind it only looks
#   deliberate on one of them.
#
#   Below 32 px the film strip goes too, and what is left is scaled up to fill
#   the tile. At 16 px the strip is four grey smudges across the camera and
#   costs more than it says; the camera and the star are the whole idea and
#   they need the room. Simplifying rather than shrinking is what icon sets do
#   at that size.
#
# Output is committed. A release job should not need librsvg or ImageMagick,
# and an icon that regenerates on every build is an icon that can silently
# change - run this when the artwork changes, look at what came out, commit it.

set -eu

cd "$(dirname "$0")/.."

command -v rsvg-convert >/dev/null 2>&1 || {
    echo "need rsvg-convert (librsvg)" >&2
    exit 1
}
if command -v magick >/dev/null 2>&1; then
    IM=magick
elif command -v convert >/dev/null 2>&1 && \
     convert -version 2>/dev/null | grep -qi imagemagick; then
    IM=convert
else
    echo "need ImageMagick" >&2
    exit 1
fi

SRC=assets/icon/film_camera_star.svg
OUT=assets/icon
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

[ -f "$SRC" ] || { echo "no $SRC" >&2; exit 1; }

python3 tools/icon-variant.py "$SRC" "$TMP/full.svg"
python3 tools/icon-variant.py "$SRC" "$TMP/small.svg" --no-strip

# The simplified artwork is rendered big, trimmed to what is actually drawn and
# re-centred, so the camera fills a 16 px tile instead of sitting in the middle
# of the space the film strip used to need. Eight per cent of margin, because a
# mark that touches the edge of its tile looks larger than its neighbours.
rsvg-convert -w 1024 -h 1024 "$TMP/small.svg" -o "$TMP/small.png"
"$IM" "$TMP/small.png" -trim +repage -resize 940x940 \
    -background none -gravity center -extent 1024x1024 "$TMP/small-fit.png"

rsvg-convert -w 1024 -h 1024 "$TMP/full.svg" -o "$TMP/full.png"

for s in 16 24 32 48 64 128 256 512; do
    case "$s" in
        16|24) from="$TMP/small-fit.png" ;;
        *)     from="$TMP/full.png" ;;
    esac
    # Lanczos down from 1024 rather than rendering the SVG at 16: librsvg
    # rounds hairlines to whole pixels at that size and the camera's outline
    # disappears in places. Downsampling keeps it as grey where it is thin,
    # which is what makes the shape still read.
    "$IM" "$from" -filter Lanczos -resize "${s}x${s}" \
        -background none -gravity center -extent "${s}x${s}" \
        -define png:color-type=6 "$OUT/icon-$s.png"
done

# 16 through 256 in the .ico. 512 is left out on purpose: Windows never asks
# for it, and every size in there is carried by every copy of the installer and
# of the executable that embeds it.
# shellcheck disable=SC2046
"$IM" $(for s in 16 24 32 48 64 128 256; do printf '%s ' "$OUT/icon-$s.png"; done) \
    "$OUT/bencsnip.ico"

for f in "$OUT"/icon-*.png "$OUT/bencsnip.ico"; do
    printf '  %-30s ' "$f"
    "$IM" identify -format '%wx%h %m\n' "$f" | head -1
done
