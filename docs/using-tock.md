# Using Tock

## Buttons

The FIRE has three buttons under the screen, **A** (left), **B** (middle) and **C** (right), and a
power key on its left side.

| Anywhere | |
| --- | --- |
| hold **B** | back to the home screen |
| hold **C** | this app's settings |
| hold the power key 2 s | off; hold it 2 s again to turn on |

On the home screen, A and C move between apps and B opens one.

## Apps

**Timer.** A countdown with Tock, in four looks (Fluid, Arcade, Bar, Blocks) and three modes:
Timer (one countdown), Pomodoro (focus and breaks, a long break after the last round; Tock has a
coffee during breaks and jumps up and down when it's time to get back to work) and Interval
(work and rest back to back, for workouts). The LEDs fill up as the time runs.

| | |
| --- | --- |
| **B** | start, pause, resume |
| **A** | next task (or pet Tock, if you have no tasks) |
| **C** | before a countdown: the next length; during one: pet Tock |
| hold **A** | reset |

**Stats.** How long you focused: the day, the week as bars, the week per task, and a 16-week
heatmap. A and C move a day back and forth, B switches views. Set the daily goal with hold C.

**Talk.** A voice conversation with OpenAI, like a walkie-talkie: hold A and talk, let go to send.
Tock thinks, answers, and waits for your next message. B stops a reply. Needs Wi-Fi and an API
key (see [Getting started](getting-started.md)); it hangs up after a minute of quiet.

**Saver.** A screensaver: your own lines, large, over a slow background. B changes the
background, A and C step through the lines. It never makes a sound.

**Settings.** Theme (dark or light), sound (off or 1 to 5), Wi-Fi and Bluetooth.

## Tasks

Tasks are optional groups to file focus time under, like STUDY or WORK. Make them in the Mac
app (Settings > TASKS), with a name and a color. On the FIRE, press A in the Timer to switch;
the current task shows as a label, and the LEDs fill in its color. Stats and the Mac split your
time by task. Removing a task keeps its time in the day totals, untagged.

## The Mac app

Tock sits in the menu bar: standing by, swaying its knob while you focus, sitting while the Timer
is paused. A dot next to it means today has no goal yet. Click it for today: the goal ring, what
the Timer is doing, what you worked on, and the last 16 weeks.

The Settings window (from the panel, or open the app again) has Tasks, Voice, Wi-Fi, Saver (the
screensaver's lines) and Mac (open at login, the log). The Mac keeps its own log of your focus in
SQLite, at `~/Library/Application Support/Tock/tock.sqlite`, whenever the FIRE is in range.
