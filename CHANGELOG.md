# Changelog

## 0.1.0 (2026-09-26)

The first version.

**FIRE firmware**

- Timer: one countdown, Pomodoro and Interval, in four looks (Fluid, Arcade, Bar, Blocks), with
  the LEDs filling as time runs. A switches task, C picks the length.
- Stats: the day, the week, the week per task, and a 16-week heatmap; a daily goal.
- Talk: push-to-talk voice conversations with an OpenAI realtime model, straight from the FIRE.
- Saver: your own lines over a slow background.
- Settings: theme, sound volume (off, 1 to 5), Wi-Fi, Bluetooth; Wi-Fi and Bluetooth state in the
  status bar.
- Bluetooth service for the Mac app, paired with a passkey ([protocol](docs/bluetooth.md)).

**Mac app**

- Menu bar only: Tock in the menu bar, moving while you focus, with today's goal, the Timer, your
  tasks and the last 16 weeks in a panel.
- Settings: tasks and their colors, the voice model and API key, Wi-Fi, the screensaver's lines,
  open at login.
- Keeps a log of your focus in SQLite whenever the FIRE is in range.
