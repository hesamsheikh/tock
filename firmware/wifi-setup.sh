#!/bin/sh
# One-time Wi-Fi setup: saves your network on the FIRE over the USB cable.
# Usage: firmware/wifi-setup.sh [serial-port]
# The password is typed here, hidden, and goes only over the cable into the FIRE's storage.
set -e
CLI="${ARDUINO_CLI:-$(command -v arduino-cli || echo "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli")}"
PORT="${1:-$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "No /dev/cu.usbserial-* port found. Is the FIRE plugged in?" >&2; exit 1; }

printf 'Wi-Fi network name: '
read -r SSID
printf 'Password (hidden): '
trap 'stty echo' EXIT INT
stty -echo
read -r PASS
stty echo
echo
case "$SSID$PASS" in *"	"*) echo "Tabs in the name or password aren't supported." >&2; exit 1 ;; esac

echo "Sending to the FIRE on $PORT and waiting for it to connect..."
( sleep 2; printf 'W%s\t%s\n' "$SSID" "$PASS"; sleep 20 ) |
  "$CLI" monitor -p "$PORT" --config 115200 --quiet 2>/dev/null |
  grep -a --line-buffered -m 2 -E "wifi (network saved|online)"
