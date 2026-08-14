#!/bin/sh
#
# Wrap the binary in a .app bundle.
#
# macOS is the one platform where the window icon cannot come from the program
# itself. Windows reads it from a resource compiled into the executable and X11
# takes it from _NET_WM_ICON, which the binary sets at startup - but GLFW's
# Cocoa backend ignores glfwSetWindowIcon entirely, because a bare Mach-O
# executable has no Finder or Dock identity to attach an icon to. The icon is a
# property of a bundle, so there has to be a bundle.
#
# Everything else the program needs is already inside the binary - the font,
# the licences, ffmpeg in a release build - which is what makes this a wrapper
# and not an install step: Contents/MacOS holds one file.
#
#   tools/macos-app.sh <binary> <output.app>
#
# Run on macOS: iconutil ships with the developer tools and has no equivalent
# elsewhere.

set -eu

BIN="$1"
APP="$2"
NAME="$(basename "$APP" .app)"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN" "$APP/Contents/MacOS/$NAME"
chmod +x "$APP/Contents/MacOS/$NAME"

# The iconset names are fixed; iconutil rejects anything it does not
# recognise. There is no 1024 px source, so 512x512@2x is simply absent - the
# set does not have to be complete, only correctly named.
ICONSET="$(mktemp -d)/icon.iconset"
mkdir -p "$ICONSET"
cp assets/icon/star-16.png  "$ICONSET/icon_16x16.png"
cp assets/icon/star-32.png  "$ICONSET/icon_16x16@2x.png"
cp assets/icon/star-32.png  "$ICONSET/icon_32x32.png"
cp assets/icon/star-64.png  "$ICONSET/icon_32x32@2x.png"
cp assets/icon/star-128.png "$ICONSET/icon_128x128.png"
cp assets/icon/star-256.png "$ICONSET/icon_128x128@2x.png"
cp assets/icon/star-256.png "$ICONSET/icon_256x256.png"
cp assets/icon/star-512.png "$ICONSET/icon_256x256@2x.png"
cp assets/icon/star-512.png "$ICONSET/icon_512x512.png"

iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/$NAME.icns"

# The paperwork goes inside the bundle. A .dmg is opened once and thrown away;
# what a person keeps is the .app they dragged out of it, and the licence that
# binary ships under has to still be attached to it then. The program shows the
# same texts in its own about window, which is what covers someone who never
# looks in here.
for f in README.md ARCHITECTURE.md LICENSE NOTICE; do
    [ -f "$f" ] && cp "$f" "$APP/Contents/Resources/"
done
[ -f vendor/ffmpeg/CONFIGURE ] && \
    cp vendor/ffmpeg/CONFIGURE "$APP/Contents/Resources/ffmpeg-configure.txt"
tools/binary-licence.sh "macos-$(uname -m)" "${SN_VERSION:-dev}" \
    "$APP/Contents/Resources/BINARY-LICENCE.txt" >/dev/null

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>$NAME</string>
    <key>CFBundleDisplayName</key>       <string>BENCsnip</string>
    <key>CFBundleExecutable</key>        <string>$NAME</string>
    <key>CFBundleIdentifier</key>        <string>net.ropple.bencsnip</string>
    <key>CFBundleIconFile</key>          <string>$NAME</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${SN_VERSION:-0.0.0}</string>
    <key>CFBundleVersion</key>           <string>${SN_VERSION:-0.0.0}</string>
    <key>LSMinimumSystemVersion</key>    <string>11.0</string>
    <!-- Without this the window is drawn at 1x and scaled up, which on a
         Retina display makes a bitmap font look exactly as bad as it sounds. -->
    <key>NSHighResolutionCapable</key>   <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key> <true/>

    <!-- Double-clicking a project opens it: the program already takes a
         .bencsnip on the command line, and this is how Finder is told that
         it does. Owner rather than viewer - this is the program that writes
         them. -->
    <key>CFBundleDocumentTypes</key>
    <array>
      <dict>
        <key>CFBundleTypeName</key>      <string>BENCsnip project</string>
        <key>CFBundleTypeRole</key>      <string>Editor</string>
        <key>LSHandlerRank</key>         <string>Owner</string>
        <key>CFBundleTypeIconFile</key>  <string>$NAME</string>
        <key>LSItemContentTypes</key>
        <array><string>net.ropple.bencsnip.project</string></array>
      </dict>
    </array>
    <key>UTExportedTypeDeclarations</key>
    <array>
      <dict>
        <key>UTTypeIdentifier</key>      <string>net.ropple.bencsnip.project</string>
        <key>UTTypeDescription</key>     <string>BENCsnip project</string>
        <key>UTTypeConformsTo</key>
        <array><string>public.plain-text</string></array>
        <key>UTTypeTagSpecification</key>
        <dict>
          <key>public.filename-extension</key>
          <array><string>bencsnip</string></array>
        </dict>
      </dict>
    </array>
</dict>
</plist>
PLIST

# Ad-hoc sign the assembled bundle.
#
# This is not about trust - an ad-hoc signature identifies nobody - it is about
# the signature being *valid*. The linker ad-hoc signs the executable on its
# own, and that signature covers the executable alone. Once the same file sits
# inside a bundle, macOS validates it against the bundle: Info.plist,
# Resources, the lot. The standalone signature does not cover any of that, so
# validation fails, and Gatekeeper reports a bundle that is perfectly intact as
# "damaged and can't be opened" - with no option to open it anyway, because
# that offer is for software it distrusts, not for software it thinks is
# corrupt.
#
# Signing here, after everything is in place, makes the signature cover what it
# is checked against. Gatekeeper then says what is actually true: unidentified
# developer, open at your own risk.
if command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - --timestamp=none "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP"
    echo "signed (ad-hoc) and verified"
else
    echo "warning: codesign not found - the bundle will be reported as damaged" >&2
fi

echo "built $APP"
