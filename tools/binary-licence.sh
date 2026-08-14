#!/bin/sh
#
# Write the BINARY-LICENCE.txt that travels with a built binary.
#
#   tools/binary-licence.sh PLATFORM VERSION OUTFILE
#
# In its own file because two things ship binaries - tools/package.sh, which
# makes the archives, and tools/macos-app.sh, which puts the same text inside
# the .app so that dragging the application out of the disk image does not
# leave the notice behind. Two copies of a licence notice is how one of them
# ends up describing a build it is not attached to.
#
# What it says depends on how ffmpeg was built, and it works that out rather
# than being told: an ffmpeg with libx264 in it makes the whole binary GPL, and
# one without leaves it under the LGPL's terms. Those are different obligations
# and must not share a paragraph.

set -eu
cd "$(dirname "$0")/.."

PLATFORM=${1:?usage: tools/binary-licence.sh PLATFORM VERSION OUTFILE}
VERSION=${2:?}
OUT=${3:?}

if grep -q 'enable-gpl' vendor/ffmpeg/CONFIGURE 2>/dev/null; then
    cat > "$OUT" <<EOF
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
  * The source is: this repository at the tag this build is named after,
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
    cat > "$OUT" <<EOF
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

echo "wrote $OUT"
