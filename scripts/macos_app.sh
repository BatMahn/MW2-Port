#!/bin/sh
# Build "MechWarrior 2.app" from a macOS build (make mw2) in the current directory (CI: the repo root).
# Layout: the binary in Contents/MacOS, the font in Contents/MacOS/assets (where the port looks: next to the
# executable), the icon (packaging/macos/mw2port.icns, an original design drawn by tools/make_icon.py) in
# Contents/Resources, and the SDL2 dylib the binary was linked against copied into Contents/Frameworks so the
# app runs without Homebrew. The bundle is ad-hoc signed (codesign -s -), not notarised: a downloaded copy
# needs  xattr -dr com.apple.quarantine "MechWarrior 2.app"  once (README).
set -e
APP="MechWarrior 2.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS/assets/fonts" "$APP/Contents/Resources"
cp mw2 "$APP/Contents/MacOS/"   # one executable: the shell and (mw2 --sim) the simulation
cp assets/fonts/LiberationSans-Bold.ttf assets/fonts/README.md "$APP/Contents/MacOS/assets/fonts/"
cp packaging/macos/mw2port.icns "$APP/Contents/Resources/"
# bundle SDL2: the first non-system dylib named libSDL2 in the binary's load commands
SDL=$(otool -L "$APP/Contents/MacOS/mw2" | awk '/libSDL2/ {print $1; exit}')
case "$SDL" in
  ""|@*) echo "SDL2 not linked from a file path ($SDL) - left as is" ;;
  *)
    mkdir -p "$APP/Contents/Frameworks"
    LIB=$(basename "$SDL")
    cp -L "$SDL" "$APP/Contents/Frameworks/$LIB"
    chmod u+w "$APP/Contents/Frameworks/$LIB"
    install_name_tool -id "@executable_path/../Frameworks/$LIB" "$APP/Contents/Frameworks/$LIB"
    install_name_tool -change "$SDL" "@executable_path/../Frameworks/$LIB" "$APP/Contents/MacOS/mw2"
    echo "bundled $SDL" ;;
esac
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>MechWarrior 2</string>
  <key>CFBundleExecutable</key><string>mw2</string>
  <key>CFBundleIdentifier</key><string>org.mw2port.mw2</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>mw2port</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
# install_name_tool voids the linker's signature; arm64 macOS refuses to run unsigned code, so re-sign ad hoc
if [ -d "$APP/Contents/Frameworks" ]; then codesign --force -s - "$APP/Contents/Frameworks/"*.dylib; fi
codesign --force -s - "$APP"
echo "built $APP (first run: open it once from a terminal with the game folder: \"$APP/Contents/MacOS/mw2\" /path/to/MW2-game)"
