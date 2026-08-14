#!/bin/sh
#
# Build a static ffmpeg into vendor/ffmpeg, so `make` produces a binary that
# needs nothing installed to run.
#
#   tools/build-ffmpeg.sh              GPL: with libx264, the usual choice
#   tools/build-ffmpeg.sh --lgpl       LGPL only: no libx264, see below
#   tools/build-ffmpeg.sh --jobs 4
#   tools/build-ffmpeg.sh --no-asm     no nasm on this machine; much slower
#   FFMPEG_VERSION=n7.1.1 tools/build-ffmpeg.sh
#
# It takes ten to thirty minutes and needs git, a compiler, make, nasm or yasm,
# and pkg-config. Nothing it downloads leaves vendor/, which is gitignored.
#
# ------------------------------------------------------------------
# Which licence, and why the default is the one it is
# ------------------------------------------------------------------
#
# H.264 is the format that plays everywhere, and the encoder for it that is
# worth having is libx264, which is GPL. Building ffmpeg with it makes the
# whole result GPL, and a binary you then distribute has to be offered under
# the GPL too. BENCsnip's own MIT terms permit that - MIT code can be
# redistributed inside a GPL work - but your obligations follow the GPL from
# there: the complete corresponding source, on request, for everything you
# shipped.
#
# --lgpl skips libx264. Decoding is unaffected - the native H.264 decoder is
# part of ffmpeg itself, so everything still opens - but the export dialog will
# fall back to libopenh264 if you built one, and to mpeg4 if not. mpeg4 in an
# mp4 plays in VLC and in most players, and looks noticeably worse per
# megabyte. Choose deliberately.
#
# Either way, the LGPL's relink requirement applies to a static build: whoever
# receives the binary must be able to replace the ffmpeg part with their own.
# The configure line this script used is written to vendor/ffmpeg/CONFIGURE for
# exactly that reason - publish it alongside your object files or a build that
# reproduces the link. See NOTICE.

set -eu

GPL=1
ASM=1
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
FFMPEG_VERSION=${FFMPEG_VERSION:-n7.1.1}
X264_VERSION=${X264_VERSION:-stable}

while [ $# -gt 0 ]; do
    case "$1" in
        --lgpl) GPL=0 ;;
        --gpl)  GPL=1 ;;
        --no-asm) ASM=0 ;;
        --jobs) JOBS=$2; shift ;;
        --jobs=*) JOBS=${1#--jobs=} ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

ROOT=$(pwd)
PREFIX=${FFMPEG_PREFIX:-$ROOT/vendor/ffmpeg}
SRC=${FFMPEG_SRC:-$ROOT/vendor/src}

# ffmpeg's build system, x264's, and every configure script they generate
# assume paths without whitespace, and fail in ways that look like something
# else entirely - an install into the first word of the path, a link error
# naming half a directory. Rather than debug that on someone's behalf, say so
# and offer the way out.
case "$PREFIX$SRC" in
    *[[:space:]]*)
        echo "The path contains a space:" >&2
        echo "    $ROOT" >&2
        echo >&2
        echo "ffmpeg and x264 cannot be built under one. Build somewhere else" >&2
        echo "and point the editor's build at the result:" >&2
        echo >&2
        echo "    FFMPEG_PREFIX=\$HOME/ffmpeg-static FFMPEG_SRC=\$HOME/ffmpeg-src \\" >&2
        echo "        tools/build-ffmpeg.sh" >&2
        echo "    make FFMPEG=\$HOME/ffmpeg-static" >&2
        exit 1
        ;;
esac

for tool in git make pkg-config; do
    command -v "$tool" >/dev/null || { echo "need $tool" >&2; exit 1; }
done
if [ "$ASM" = 1 ] && ! command -v nasm >/dev/null && ! command -v yasm >/dev/null; then
    echo "need nasm or yasm to build ffmpeg's assembly." >&2
    echo "install one, or pass --no-asm and accept a decoder several times" >&2
    echo "slower - which on a 4K timeline is the difference between a preview" >&2
    echo "that keeps up and one that does not." >&2
    exit 1
fi

mkdir -p "$SRC" "$PREFIX"

# ------------------------------------------------------------------
# x264
# ------------------------------------------------------------------
if [ "$GPL" = 1 ]; then
    if [ ! -d "$SRC/x264" ]; then
        git clone --depth 1 --branch "$X264_VERSION" \
            https://code.videolan.org/videolan/x264.git "$SRC/x264"
    fi
    # Arguments go in the positional parameters rather than in one string.
    # A string has to be expanded unquoted to become several arguments, and
    # that splits a --prefix containing a space into two of them - which
    # installs the library into the first word of the path and reports
    # success.
    #
    # x264 spells it --disable-asm; ffmpeg spells the same idea
    # --disable-x86asm. Passing one to the other is a configure error rather
    # than a warning, which is a slow way to find out.
    set -- --prefix="$PREFIX" --enable-static --enable-pic --disable-cli
    if [ "$ASM" = 0 ]; then set -- "$@" --disable-asm; fi
    (
        cd "$SRC/x264"
        ./configure "$@"
        make -j"$JOBS"
        make install
    )
fi

# ------------------------------------------------------------------
# ffmpeg
#
# Trimmed to what BENCsnip links: no programs, no documentation, no device
# capture, no filter graph and no network protocols. The editor opens local
# files and does its own scaling, mixing and compositing, so avfilter and
# avdevice are several megabytes of code nothing calls. Everything that decides
# what a file *is* stays in: every demuxer, every decoder, every parser.
# ------------------------------------------------------------------
if [ ! -d "$SRC/ffmpeg" ]; then
    git clone --depth 1 --branch "$FFMPEG_VERSION" \
        https://git.ffmpeg.org/ffmpeg.git "$SRC/ffmpeg"
fi

# The autodetected extras are turned off by hand, and that is the whole point
# of building a static ffmpeg: configure finds bzip2, lzma, VAAPI, VDPAU and
# libdrm on the build machine, links them, and the "self-contained" binary then
# refuses to start on a machine missing libva.so.2. None of them are used here
# - decoding is software, and the two compressors only serve container corners
# - so what is left needing a shared library is OpenGL, X11 and libc.
#
# zlib stays: matroska, mov and png headers want it, it is on every system that
# has ever run anything, and ffmpeg without it quietly loses formats.
set -- --prefix="$PREFIX" \
    --disable-shared --enable-static --enable-pic \
    --disable-programs --disable-doc --disable-htmlpages --disable-manpages \
    --disable-avdevice --disable-avfilter --disable-postproc \
    --disable-network --disable-debug --enable-small \
    --disable-bzlib --disable-lzma --disable-iconv \
    --disable-vaapi --disable-vdpau --disable-libdrm \
    --disable-v4l2-m2m --disable-xlib --disable-sdl2 \
    --disable-alsa --disable-libxcb

if [ "$ASM" = 0 ]; then set -- "$@" --disable-x86asm; fi
if [ "$GPL" = 1 ]; then set -- "$@" --enable-gpl --enable-libx264; fi

(
    cd "$SRC/ffmpeg"
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" ./configure "$@"
    make -j"$JOBS"
    make install
)

# The exact line, kept where a person looking for it will find it. See the
# licence note at the top of this file.
{
    echo "# Written by tools/build-ffmpeg.sh"
    echo "# ffmpeg $FFMPEG_VERSION"
    if [ "$GPL" = 1 ]; then
        echo "# x264 $X264_VERSION (GPL: the resulting binary is GPL as a whole)"
    fi
    printf 'configure'
    for arg in "$@"; do printf ' %s' "$arg"; done
    echo
} > "$PREFIX/CONFIGURE"

echo
echo "built into vendor/ffmpeg"
echo "the configure line is in vendor/ffmpeg/CONFIGURE"
echo
echo "now:  make clean && make && make info"
