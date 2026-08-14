#!/bin/sh
#
# Wrap a built binary into the archive a release publishes.
#
#   tools/package.sh PLATFORM [VERSION]
#   tools/package.sh linux-x86_64 v0.1.0
#
# One script for all three platforms so that what is inside an archive does not
# depend on which job built it. Called by .github/workflows/release.yml, and by
# hand when checking what a release would contain.
#
# Windows gets a zip because Windows has no executable bit to lose. Everything
# else gets a tar: the zip format as GitHub writes it drops the mode, so a
# downloaded binary would need a chmod before it would start, which nobody
# expects and nothing tells them.

set -eu
cd "$(dirname "$0")/.."

PLATFORM=${1:?usage: tools/package.sh PLATFORM [VERSION]}
VERSION=${2:-$(git describe --tags --always 2>/dev/null || echo dev)}
VERSION=${VERSION#refs/tags/}

BIN=bencsnip
case "$PLATFORM" in *windows*) BIN=bencsnip.exe ;; esac
[ -f "$BIN" ] || { echo "no $BIN - run make first" >&2; exit 1; }

D="bencsnip-$VERSION-$PLATFORM"
rm -rf "$D" dist
mkdir -p "$D" dist

cp "$BIN" README.md ARCHITECTURE.md LICENSE NOTICE "$D"/

# What the binary is actually under, said in the archive rather than only in a
# release note nobody keeps. A static ffmpeg with libx264 in it makes the whole
# thing GPL; a build without one does not, and the two cases must not be
# described by the same file.
if [ -f vendor/ffmpeg/CONFIGURE ]; then
    cp vendor/ffmpeg/CONFIGURE "$D/ffmpeg-configure.txt"
fi

if grep -q 'enable-gpl' vendor/ffmpeg/CONFIGURE 2>/dev/null; then
    cat > "$D/BINARY-LICENCE.txt" <<EOF
BENCsnip $VERSION - $PLATFORM

BENCsnip's own source is under the MIT licence; LICENSE is that licence and
NOTICE lists everything else in here.

THIS BINARY IS UNDER THE GPL. It statically links FFmpeg built with libx264,
and libx264 is GPL v2 or later, which makes the work as a whole GPL. That is a
property of this build, not of the project: an FFmpeg built without libx264
leaves the binary under the LGPL's terms instead, and tools/build-ffmpeg.sh
--lgpl produces one.

What that obliges whoever redistributes this binary to do:

  * Pass on these terms, and offer the complete corresponding source.
  * The source is: this repository at the tag this archive is named after,
    <https://github.com/bropple/BENCsnip>, together with FFmpeg and x264 at the
    versions named in ffmpeg-configure.txt, which also records the exact
    configure line they were built with.
  * The build is reproducible from that: tools/build-ffmpeg.sh, then make.

FFmpeg is copyright (c) 2000-2026 the FFmpeg developers.
x264 is copyright (c) 2003-2026 x264 project.
raylib is copyright (c) 2013-2026 Ramon Santamaria, under zlib/libpng.
Terminus (TTF) is bundled unmodified under the SIL Open Font License; the
licence text is compiled into the program and shown in its about window.
EOF
else
    cat > "$D/BINARY-LICENCE.txt" <<EOF
BENCsnip $VERSION - $PLATFORM

BENCsnip's own source is under the MIT licence; LICENSE is that licence and
NOTICE lists everything else in here.

This binary links FFmpeg under the LGPL v2.1 or later. Where FFmpeg is linked
statically - see ffmpeg-configure.txt, if it is present - whoever redistributes
this must be able to relink it against their own FFmpeg: publish the object
files or a build that reproduces the link, alongside the configure line in that
file.

FFmpeg is copyright (c) 2000-2026 the FFmpeg developers.
raylib is copyright (c) 2013-2026 Ramon Santamaria, under zlib/libpng.
Terminus (TTF) is bundled unmodified under the SIL Open Font License; the
licence text is compiled into the program and shown in its about window.
EOF
fi

case "$PLATFORM" in
    *windows*)
        zip -qr "dist/$D.zip" "$D"
        ;;
    *)
        tar czf "dist/$D.tar.gz" "$D"
        ;;
esac

rm -rf "$D"
ls -l dist/
