# Getting started

You need:

- an **M5Stack FIRE** (the ESP32 core with the M5GO base: its LED strips and microphone are used)
- a **Mac**: the build scripts are macOS scripts, and the companion app needs macOS 15 or later
- a USB-C cable

## 1. Flash the firmware

Install the Arduino command line tool. Either one works:

- [Arduino IDE 2](https://www.arduino.cc/en/software), which ships `arduino-cli` inside it (the
  flash script finds it there), or
- `brew install arduino-cli`

On Apple silicon without Rosetta you also need a native `esptool` (`pip install esptool` or
`brew install esptool`); the ESP32 core's own copy is Intel-only.

Then plug in the FIRE and run:

    firmware/flash.sh

The first run downloads the pinned ESP32 core (2.1.4) and libraries from `firmware/tock/sketch.yaml`,
which takes a few minutes. The script picks the first `/dev/cu.usbserial-*` port; pass another
port as the first argument if you have more than one. If no port shows up, install the
[Silicon Labs CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).

Optional: copy `firmware/tock/secrets.h.example` to `firmware/tock/secrets.h` and set your time
zone (the default is UTC). Stats uses it to know when your day starts.

## 2. Install the Mac app

    companion/build.sh install

This builds Tock with the Command Line Tools (no Xcode needed), puts it in `/Applications`, and
starts it. It lives in the menu bar, not the Dock, and opens at login from then on.

## 3. Pair

1. On the FIRE: open **Settings** and switch **BLUETOOTH** on.
2. macOS asks once whether Tock may use Bluetooth: allow it.
3. The FIRE shows a six-digit code; type it into the box macOS opens.

From then on the Mac and the FIRE find each other on their own. The app sets the FIRE's clock
every time it connects (the FIRE has no clock of its own).

## 4. Wi-Fi and Talk (optional)

Wi-Fi is only needed for Talk and for setting the clock without the Mac.

- **Wi-Fi**: in the Mac app, Settings > WI-FI, type your network and password. (Without the
  app: `firmware/wifi-setup.sh`, over the USB cable.) The ESP32 only joins 2.4 GHz networks.
- **Talk**: in Settings > VOICE, paste an [OpenAI API key](https://platform.openai.com/api-keys)
  and pick a model. The key is sent to the FIRE and kept there; the Mac doesn't store it.

## Updates

Once it's set up, you don't need the cable again. The Mac app checks for a new release once a
day: when there is one, the menu bar panel says so, and UPDATE brings both up to date. The FIRE
goes first, over Bluetooth, in about two minutes (keep it close, and switched on); then the app
restarts as the new version. You can also check yourself in Settings > MAC.

Next: [Using Tock](using-tock.md).
