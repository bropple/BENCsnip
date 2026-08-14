#!/bin/sh
#
# One version, everywhere.
#
# src/core/sn_version.h says it is the single place the window title, the
# information window and anything that packages a build read from. This is what
# keeps that true: nothing else may spell a version out, and on a release the
# tag has to agree with it.
#
#   tools/check-version.sh            consistency
#   tools/check-version.sh v0.1.1     consistency, and that the tag agrees
#
# The failure this exists to prevent is quiet: an archive named v0.2.0 holding
# a binary whose about box says 0.1.0, which nobody notices until someone
# reports a bug against the wrong version.

set -eu
cd "$(dirname "$0")/.."

H=src/core/sn_version.h
num() { sed -n "s/^#define $1 *\([0-9][0-9]*\).*/\1/p" "$H"; }

V="$(num SN_VERSION_MAJOR).$(num SN_VERSION_MINOR).$(num SN_VERSION_PATCH)"
case "$V" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "cannot read a version out of $H (got '$V')"; exit 1 ;;
esac
echo "  sn_version.h  $V"

fail=0

# A three-number literal anywhere in the sources is a second place to forget.
# The header itself is where they live, so it is not searched.
for f in $(git ls-files 'src/*.cpp' 'src/*.h' 'tools/*.c' 'tools/*.cpp' 2>/dev/null); do
    [ "$f" = "$H" ] && continue
    if grep -nE '"[0-9]+\.[0-9]+\.[0-9]+"' "$f"; then
        echo "  $f has a version literal - it should use SN_VERSION"
        fail=1
    fi
done
[ "$fail" = 0 ] && echo "  ok            no version literals in the sources"

# The tag, when there is one.
if [ $# -gt 0 ]; then
    want=${1#v}
    if [ "$want" != "$V" ]; then
        echo
        echo "  tag $1 does not match $H ($V)."
        echo "  bump the header, or tag v$V."
        fail=1
    else
        echo "  ok            tag $1"
    fi
fi

[ "$fail" = 0 ] || exit 1
echo "  one version, everywhere: $V"
