#!/bin/sh
# Build Tock and flash it to the FIRE over USB.
# Usage: firmware/flash.sh [serial-port]   (defaults to the first /dev/cu.usbserial-*)
#        firmware/flash.sh --build-only    just build, into firmware/build/
#        firmware/flash.sh --release       build for a release: like --build-only, without secrets.h
#
# The build is pinned in tock/sketch.yaml (ESP32 core 2.1.4 and exact library versions);
# arduino-cli installs that set on first run, apart from any Arduino IDE setup.
set -e
cd "$(dirname "$0")"

CLI="${ARDUINO_CLI:-$(command -v arduino-cli || echo "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli")}"
BUILD_ONLY=; FLAGS=
[ "$1" = "--build-only" ] && BUILD_ONLY=1
[ "$1" = "--release" ] && BUILD_ONLY=1 && FLAGS=-DTOCK_RELEASE
# Tock only takes connections: leave out NimBLE's client and scanner (about 30 KB)
BLE="-DCONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED -DCONFIG_BT_NIMBLE_ROLE_OBSERVER_DISABLED"
if [ -z "$BUILD_ONLY" ]; then
  PORT="${1:-$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)}"
  [ -n "$PORT" ] || { echo "No /dev/cu.usbserial-* port found. Is the FIRE plugged in?" >&2; exit 1; }
fi

# Core 2.1.4 ships Intel-only ctags and esptool binaries. On Apple silicon without Rosetta:
# skip ctags (Tock declares every function before use), and use a native esptool instead.
ESPTOOL="$(command -v esptool || command -v esptool.py || true)"
if [ -z "$ESPTOOL" ]; then
  for e in "$HOME"/Library/Arduino15/packages/*/tools/esptool_py/*/esptool; do
    if file "$e" | grep -q "$(uname -m)"; then ESPTOOL="$e"; fi
  done
fi
[ -n "$ESPTOOL" ] || { echo "Need a native esptool: pip install esptool (or brew install esptool)" >&2; exit 1; }

# the sprite header is checked in; it is only regenerated where the pose sources (lab/) exist
if [ -f ../lab/tock.js ] && command -v node >/dev/null; then node gen-sprites.js; fi
"$CLI" compile --profile fire \
  --build-property "tools.ctags.pattern=/usr/bin/true" \
  --build-property "tools.esptool_py.path=$(dirname "$ESPTOOL")" \
  --build-property "tools.esptool_py.cmd=$(basename "$ESPTOOL")" \
  --build-property "compiler.cpp.extra_flags=$FLAGS $BLE" \
  --build-property "compiler.c.extra_flags=$BLE" \
  --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=psram_enable" \
  --output-dir build tock
[ -n "$BUILD_ONLY" ] && exit 0

PLATFORM="$("$CLI" compile --profile fire --show-properties tock 2>/dev/null | sed -n 's/^runtime.platform.path=//p')"
"$ESPTOOL" --chip esp32 --port "$PORT" --baud 1500000 write-flash -z \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0x1000 build/tock.ino.bootloader.bin \
  0x8000 build/tock.ino.partitions.bin \
  0xe000 "$PLATFORM/tools/partitions/boot_app0.bin" \
  0x10000 build/tock.ino.bin
