#!/bin/sh
# Build "MechWarrior 2.app" from a macOS build (make mw2 gl). UNTESTED here (no macOS available while writing it):
# the layout is the standard one - binaries in Contents/MacOS, the font in Contents/MacOS/assets (where the port
# looks: next to the executable), SDL2 from Homebrew or a framework next to the binaries.
set -e
APP="MechWarrior 2.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS/assets/fonts" "$APP/Contents/Resources"
cp mw2 "$APP/Contents/MacOS/"   # one executable: the shell and (mw2 --sim) the simulation
cp assets/fonts/LiberationSans-Bold.ttf assets/fonts/README.md "$APP/Contents/MacOS/assets/fonts/"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>MechWarrior 2</string>
  <key>CFBundleExecutable</key><string>mw2</string>
  <key>CFBundleIdentifier</key><string>org.mw2port.mw2</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
echo "built $APP (first run: open it once from a terminal with the game folder: \"$APP/Contents/MacOS/mw2\" /path/to/MW2-game)"
