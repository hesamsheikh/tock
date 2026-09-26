#!/bin/sh
# Build Tock.app (no Xcode needed, just the Command Line Tools) into companion/build/.
# `build.sh install` also puts it in /Applications and starts it there: Tock opens at login from
# where it was first started, so that should be somewhere that stays put.
set -e
cd "$(dirname "$0")"
APP=build/Tock.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
swiftc -O -swift-version 5 -parse-as-library -target "$(uname -m)-apple-macos15.0" \
  Sources/*.swift -o "$APP/Contents/MacOS/Tock"
cp Info.plist "$APP/Contents/"
if [ ! -f build/AppIcon.icns ]; then
  swift make-icon.swift
  for s in 16 32 128 256 512; do
    sips -z $s $s build/icon-1024.png --out build/AppIcon.iconset/icon_${s}x${s}.png >/dev/null
    sips -z $((s * 2)) $((s * 2)) build/icon-1024.png --out build/AppIcon.iconset/icon_${s}x${s}@2x.png >/dev/null
  done
  iconutil -c icns build/AppIcon.iconset -o build/AppIcon.icns
fi
cp build/AppIcon.icns "$APP/Contents/Resources/"
# Ad-hoc signed, but identified by bundle id rather than by the build's hash: macOS keys the
# Bluetooth permission to this, so it survives rebuilds instead of being asked for (and quietly
# withheld) every time.
codesign --force --sign - -r='designated => identifier "dev.tock.companion"' "$APP" >/dev/null
echo "built $APP"
if [ "$1" = "install" ]; then
  pkill -x Tock 2>/dev/null || true
  sleep 1
  rm -rf /Applications/Tock.app
  ditto "$APP" /Applications/Tock.app
  open /Applications/Tock.app
  echo "installed /Applications/Tock.app"
fi
