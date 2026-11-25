#!/bin/sh
# Build "MechWarrior 2.app" from a macOS build (make mw2) in the current directory (CI: the repo root), and
# "MW2 Map Viewer.app" from make mapview when that binary is there.
# Layout: the binary in Contents/MacOS, the font in Contents/Resources/assets (SDL_GetBasePath() returns
# Contents/Resources inside a bundle; codesign rejects non-code files in Contents/MacOS), the icon (packaging/macos/mw2port.icns, made from the box art by tools/make_icon.py) in
# Contents/Resources, and the SDL3 dylib the binary was linked against (Homebrew's sdl3) copied into Contents/Frameworks so
# the app runs without Homebrew (the graphics are Metal through SDL3's GPU API: nothing else to bundle). The bundle is ad-hoc signed (codesign -s -), not notarised: a downloaded copy
# needs  xattr -dr com.apple.quarantine "MechWarrior 2.app"  once (README).
set -e
VERSION=${GITHUB_REF_NAME#v}   # the release tag in CI, or the version below
[ -n "$VERSION" ] || VERSION=2.0

# bundle APP BINARY BUNDLE_ID NAME
bundle() {
  APP=$1; BIN=$2; ID=$3; NAME=$4
  rm -rf "$APP"
  mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/assets/fonts"
  cp "$BIN" "$APP/Contents/MacOS/"
  cp assets/fonts/LiberationSans-Bold.ttf assets/fonts/README.md "$APP/Contents/Resources/assets/fonts/"
  cp packaging/macos/mw2port.icns "$APP/Contents/Resources/"
  cp LICENSE THIRD_PARTY.md "$APP/Contents/Resources/"   # the licences (GPL-2.0-or-later; third-party components)
  # bundle SDL3: the dylib named libSDL3 in the binary's load commands (Homebrew: /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib;
  # an SDL3 built from source links @rpath/libSDL3.0.dylib - found through pkg-config's libdir then)
  if otool -L "$APP/Contents/MacOS/$BIN" | grep -q 'libSDL2'; then
    echo "$BIN links SDL2: rebuild it with SDL3 (brew install sdl3 pkgconf; make clean; make $BIN)" >&2; exit 1
  fi
  SDL=$(otool -L "$APP/Contents/MacOS/$BIN" | awk '/libSDL3/ {print $1; exit}')
  case "$SDL" in
    "") echo "$BIN does not link libSDL3 (an SDL3.framework or a static SDL3?) - left as is" ;;
    *)
      SRC=$SDL
      case "$SDL" in
        @rpath/*) SRC="$(${PKG_CONFIG:-pkg-config} --variable=libdir sdl3)/${SDL#@rpath/}" ;;
        @*) SRC="" ;;
      esac
      [ -n "$SRC" ] && [ -f "$SRC" ] || { echo "cannot find the SDL3 library $SDL" >&2; exit 1; }
      mkdir -p "$APP/Contents/Frameworks"
      LIB=$(basename "$SDL")
      cp -L "$SRC" "$APP/Contents/Frameworks/$LIB"
      chmod u+w "$APP/Contents/Frameworks/$LIB"
      install_name_tool -id "@executable_path/../Frameworks/$LIB" "$APP/Contents/Frameworks/$LIB"
      install_name_tool -change "$SDL" "@executable_path/../Frameworks/$LIB" "$APP/Contents/MacOS/$BIN"
      echo "bundled $SRC into $APP" ;;
  esac
  cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleExecutable</key><string>$BIN</string>
  <key>CFBundleIdentifier</key><string>$ID</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>mw2port</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>10.14</string>
</dict></plist>
PLIST
  # install_name_tool voids the linker's signature; arm64 macOS refuses to run unsigned code, so re-sign ad hoc
  if [ -d "$APP/Contents/Frameworks" ]; then codesign --force -s - "$APP/Contents/Frameworks/"*.dylib; fi
  codesign --force -s - "$APP"
  echo "built $APP"
}

bundle "MechWarrior 2.app" mw2 org.mw2port.mw2 "MechWarrior 2"   # one executable: the shell and (mw2 --sim) the simulation
echo "(on first launch it finds MW2-game next to the app or in ~, or asks for the game folder)"
if [ -f mapview ]; then
  bundle "MW2 Map Viewer.app" mapview org.mw2port.mapview "MW2 Map Viewer"   # the map viewer (tools/mapview.c)
fi
