# Firmware

Arduino C++ for the ESP32, in `firmware/tock/`. Built with `arduino-cli` against the profile in
`sketch.yaml`: ESP32 core 2.1.4 on purpose (with core 3.x, Wi-Fi, Bluetooth and PSRAM together
overflow the ESP32's IRAM), M5Unified and M5GFX for the board, NimBLE for Bluetooth, and
Adafruit NeoPixel for the LEDs.

The frame is 8-bit (each color gets a palette slot the first time it's drawn), in four strips of
60 rows kept in internal RAM, and only the rows that changed go to the display: most frames take
a few milliseconds, and the loop runs at up to 60 fps. While Talk or an update runs, they need
that internal RAM, so the strips move to PSRAM, and back after, each where there's room. Talk and
updates run at 20 fps (10 while Talk connects): PSRAM shares a bus with the flash the code runs
from, and drawing there flat out slowed Talk's TLS handshake past its timeout. `flash.sh` also leaves out the parts of
NimBLE Tock doesn't use (the client and the scanner).

PSRAM runs at 40 MHz, not the core's 80: at 80 the FIRE's PSRAM hands back words shifted by a
nibble, hundreds a minute, which garbled Talk's audio and messages. `flash.sh` links with
`--wrap=psram_enable` and `tock.ino` passes the slower mode on; build with `flash.sh` so that's
in. Serial `P` checks it.

| File | |
| --- | --- |
| `tock.ino` | setup, the main loop, button events, serial debug commands |
| `system.h` | sound, preferences (NVS), LEDs, the clock, the focus log, the app interface |
| `gfx.h` | the palette (dark and light), the 3 x 5 pixel font, drawing, sending frames to the display |
| `sprites.h`, `tock_sprites.h` | Tock's poses |
| `home.h` | the home screen and the settings sheet every app gets on hold C |
| `timer.h`, `stats.h`, `talk.h`, `saver.h`, `settings.h` | the apps |
| `tasks.h` | task groups and the screensaver's lines |
| `net.h` | Wi-Fi, and the Bluetooth service the Mac app talks to ([protocol](bluetooth.md)) |
| `update.h` | firmware updates from the Mac app, and their screen |
| `power.h` | hold-to-turn-off and hold-to-turn-on with the IP5306 power chip |
| `mic.h`, `voice.h` | the microphone and the speaker for Talk |
| `ca.h` | root certificates for api.openai.com |

## Build and flash

    firmware/flash.sh [serial-port]
    firmware/flash.sh --build-only     # just build, into firmware/build/
    firmware/flash.sh --release        # the same, without secrets.h (the release workflow uses this)

See [Getting started](getting-started.md) for what it needs. `ARDUINO_CLI` overrides where the
script looks for `arduino-cli`.

The version is `TOCK_VERSION` in `system.h`. Once a FIRE runs 0.1.0 or later, new versions reach
it from the Mac app over Bluetooth ([Releasing](releasing.md)).

## Serial debug

At 115200 baud (`arduino-cli monitor -p <port> --config 115200`), single keys drive the FIRE:

| Key | |
| --- | --- |
| `a` `b` `c` | tap A, B, C |
| `A` `B` `C` | hold A, B, C |
| `(` `)` | press / release A (Talk's push-to-talk) |
| `s` | dump a screenshot (run-length encoded RGB565) |
| `r` | status: power chip, Wi-Fi, Bluetooth, clock, memory, and where a frame's time goes |
| `x` | countdowns at 1x, 10x, 60x |
| `T<epoch>` + newline | set the clock (UTC seconds) |
| `W<ssid><tab><password>` + newline | save a Wi-Fi network |
| `K<id>\|<color>\|<name>;...` + newline | set the task list |
| `M<line>;<line>;...` + newline | set the screensaver's lines |
| `w` | scan for Wi-Fi networks |
| `L` | Bluetooth off and on |
| `m` / `v` | microphone test / speaker test (silent) |
| `d` / `D` | fill the focus log with demo history / wipe it |
| `P` | PSRAM test: 1 MB checked every 5 s for a minute (should stay at 0) |

Opening the port with most serial tools resets the ESP32; `arduino-cli monitor` does not.
