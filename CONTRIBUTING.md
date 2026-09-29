# Contributing

Thanks for wanting to help with Tock.

Tock is a side project. I work on it in my spare time, when I have the time and the energy, and
that's what keeps it fun. So there's no schedule and no promise of support: issues and pull
requests are welcome and I do read them, but replies can take a while, and not every idea will
make it in, even good ones. Please don't take the quiet personally. And if you need something
sooner, or different, fork it: that's what the [MIT license](LICENSE) is for.

## Before you start

- **Bugs**: open an issue with what happened and what you expected, the versions (Settings > MAC
  in the Mac app shows both), and for the FIRE, the serial `r` output if you can get it.
- **Small fixes** (docs, typos, an obvious bug): send a pull request.
- **Bigger changes** (a new app or screen, a change to the Bluetooth protocol, a new library):
  open an issue first, so we can talk it through before you spend the time.

## Setup

You need a FIRE and a Mac with macOS 15 or later, plus the tools in
[Getting started](docs/getting-started.md): `arduino-cli` (or Arduino IDE 2), a native `esptool`
on Apple silicon, and the Command Line Tools. The first firmware build downloads the pinned ESP32
core and libraries, which takes a few minutes.

## Where things are

    firmware/tock/              the firmware: docs/firmware.md has a table of every file
    firmware/flash.sh           builds the firmware, and flashes it over USB
    firmware/wifi-setup.sh      saves a Wi-Fi network on the FIRE, over USB
    firmware/gen-sprites.js     regenerates tock_sprites.h (see below)
    companion/Sources/          the Mac app (table below)
    companion/build.sh          builds the Mac app, and installs it
    companion/make-icon.swift   draws the app icon (build.sh runs it)
    .github/workflows/          the release workflow (docs/releasing.md)
    docs/                       how Tock works, and how to use it

The Mac app:

| File | |
| --- | --- |
| `App.swift` | the app, and the Settings window: tasks, voice, Wi-Fi, saver, Mac |
| `MenuBar.swift` | Tock in the menu bar, and its panel |
| `TockLink.swift` | the Bluetooth link to the FIRE ([protocol](docs/bluetooth.md)) |
| `Store.swift` | the Mac's log of your focus, in SQLite |
| `Updater.swift` | updates for the app and the FIRE, from GitHub releases |
| `Pixel.swift` | the palette, the pixel font and Tock's poses, drawn as on the FIRE |

## Working on the firmware

    firmware/flash.sh                                               # build and flash
    arduino-cli monitor -p /dev/cu.usbserial-XXXX --config 115200   # then drive it from here

The serial monitor lets you press the FIRE's buttons from the keyboard and read its state; the
keys are in [Firmware](docs/firmware.md#serial-debug). `r` shows memory and frame times, `P`
tests PSRAM.

- Always build with `flash.sh`, not from the Arduino IDE or a bare `arduino-cli compile`. It pins
  the ESP32 core and libraries, and adds flags the firmware needs: PSRAM at 40 MHz (at 80, the
  FIRE's PSRAM corrupts data), NimBLE without the parts Tock doesn't use, and your home folder's
  path kept out of the binary.
- The core stays at 2.1.4 on purpose: with 3.x, Wi-Fi, Bluetooth and PSRAM together overflow the
  ESP32's IRAM.
- Internal RAM is tight: Talk and updates need most of it. If a change keeps memory around,
  compare `r`'s internal RAM line before and after, and try Talk.
- `firmware/tock/secrets.h` (optional, and git ignores it) holds settings for your FIRE only:
  see `secrets.h.example`.
- Tock's poses are in `tock_sprites.h`, generated from pose sketches that aren't in the repo. If
  you change a pose, edit that file and say so in the pull request, so it can be carried back.

## Working on the Mac app

    companion/build.sh install     # build, put it in /Applications, and start it

- To read its log (Bluetooth events and updates, as `tock: ...` lines), quit it, start it with
  `open --stdout /tmp/tock.log --stderr /tmp/tock.log /Applications/Tock.app`, and follow that
  file. Don't run the binary straight from a terminal: macOS then asks for Bluetooth on the
  terminal's behalf, and the app gets none.
- If Bluetooth goes silent after a rebuild (no errors, and the FIRE never sees a connection),
  macOS has dropped the app's Bluetooth permission. `build.sh` signs the app so the permission
  survives rebuilds: keep its `codesign` line and the bundle id as they are. To confirm, watch
  `/usr/bin/log stream --predicate 'process == "tccd"'` for "Failed to match existing code
  requirement" (in zsh, a plain `log` is a shell builtin).
- `--demo` runs without a FIRE, on made-up data. Testing updates, and sending a firmware straight
  to the FIRE: [Tock for Mac](companion/README.md).
- A change to what the FIRE and the Mac say to each other goes in both, and in
  [docs/bluetooth.md](docs/bluetooth.md).

## Before you open a pull request

- Both build: `firmware/flash.sh --build-only` and `companion/build.sh`.
- You ran it: the firmware on a FIRE, the app on a Mac, and the two together if your change
  touches both. Say in the pull request what you tried.
- The docs match: if behavior changed, the docs that describe it (and the file tables) say so.
- Anything a user would notice has a line in `CHANGELOG.md`, under `## Unreleased` at the top
  (add that heading if it isn't there).
- The version stays as it is: it changes when a release is made.
- One change per pull request.

## Style

Match the code around your change. Comments say why, in plain sentences, and lines stay within
about 100 columns. No new libraries or tools without talking about it first: the build is pinned
(`firmware/tock/sketch.yaml`) so that it builds the same everywhere. Keep docs short and plain,
like the ones here.

## Releases

I make the releases ([Releasing](docs/releasing.md)): a pushed version tag builds and publishes
them with GitHub Actions, and the Mac app offers them to everyone.

## License

By contributing, you agree that your work is under the [MIT license](LICENSE), like the rest of
Tock.
