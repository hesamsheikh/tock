# Tock for Mac

A menu bar companion for the FIRE (no Dock icon). It pairs over Bluetooth, starts at login and
keeps running, so whenever Tock is in range it:

- shows today in the menu bar: Tock standing by, swaying its knob while you focus, sitting while
  the Timer is paused, and a dot until today's goal is set
- keeps your focus log on this Mac, in SQLite (`~/Library/Application Support/Tock/tock.sqlite`):
  day totals, time per task, daily goals, and each focus and break session it sees
- sets Tock's clock (the FIRE has no clock of its own)

Click Tock in the menu bar for today on glass: the goal ring filled in your tasks' colors, today's
goal (any length), what the Timer is doing, what you worked on, and the last 16 weeks.

Settings (from the panel, or cmd-comma there, or by opening the app again) has five tabs:

- Tasks: make task groups with a name and a color, and see this week split by them
- Voice: the OpenAI API key and model for Talk (the key is kept on Tock, not on this Mac)
- Wi-Fi: Tock's network
- Saver: the screensaver's lines
- Mac: open at login, and the log

Light or dark follows the Mac.

## Build and install

Needs only the Command Line Tools (no Xcode), and macOS 15 or later:

    companion/build.sh install     # builds, puts Tock in /Applications and starts it
    companion/build.sh             # just builds, into companion/build/

For screenshots, without Bluetooth:

    open -n companion/build/Tock.app --args --demo --panel --settings

## Pairing

1. On Tock: Settings, switch BLUETOOTH on.
2. Open the app. macOS asks once whether Tock may use Bluetooth.
3. Tock shows a six-digit code; type it into the box macOS opens.

After that the Mac and Tock remember each other and reconnect on their own.
The Bluetooth protocol is described at the top of `firmware/tock/net.h`.
