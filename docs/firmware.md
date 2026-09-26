# Firmware

Arduino C++ for the ESP32, in `firmware/tock/`. Built with `arduino-cli` against the profile in
`sketch.yaml`: ESP32 core 2.1.4 on purpose (with core 3.x, Wi-Fi, Bluetooth and PSRAM together
overflow the ESP32's IRAM), M5Unified and M5GFX for the board, NimBLE for Bluetooth, and
Adafruit NeoPixel for the LEDs.

| File | |
| --- | --- |
| `tock.ino` | setup, the main loop, button events, serial debug commands |
| `system.h` | sound, preferences (NVS), LEDs, the clock, the focus log, the app interface |
| `gfx.h` | the palette (dark and light), the 3 x 5 pixel font, drawing |
| `sprites.h`, `tock_sprites.h` | Tock's poses |
| `home.h` | the home screen and the settings sheet every app gets on hold C |
| `timer.h`, `stats.h`, `talk.h`, `saver.h`, `settings.h` | the apps |
| `tasks.h` | task groups and the screensaver's lines |
| `net.h` | Wi-Fi, and the Bluetooth service the Mac app talks to ([protocol](bluetooth.md)) |
| `power.h` | hold-to-turn-off and hold-to-turn-on with the IP5306 power chip |
| `mic.h`, `voice.h` | the microphone and the speaker for Talk |
| `ca.h` | root certificates for api.openai.com |

## Build and flash

    firmware/flash.sh [serial-port]

See [Getting started](getting-started.md) for what it needs. `ARDUINO_CLI` overrides where the
script looks for `arduino-cli`.

## Serial debug

At 115200 baud (`arduino-cli monitor -p <port> --config 115200`), single keys drive the FIRE:

| Key | |
| --- | --- |
| `a` `b` `c` | tap A, B, C |
| `A` `B` `C` | hold A, B, C |
| `(` `)` | press / release A (Talk's push-to-talk) |
| `s` | dump a screenshot (run-length encoded RGB565) |
| `r` | status: power chip, Wi-Fi, Bluetooth, clock, memory |
| `x` | countdowns at 1x, 10x, 60x |
| `T<epoch>` + newline | set the clock (UTC seconds) |
| `W<ssid><tab><password>` + newline | save a Wi-Fi network |
| `K<id>\|<color>\|<name>;...` + newline | set the task list |
| `M<line>;<line>;...` + newline | set the screensaver's lines |
| `w` / `l` | scan for Wi-Fi networks / Bluetooth devices |
| `L` | Bluetooth off and on |
| `m` / `v` | microphone test / speaker test (silent) |
| `d` / `D` | fill the focus log with demo history / wipe it |

Opening the port with most serial tools resets the ESP32; `arduino-cli monitor` does not.
