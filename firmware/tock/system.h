// system.h: what every screen can use.
//   Sound   M5.Speaker beeps, with a small queue for melodies, silenced for quiet screens
//   Prefs   small saved numbers (NVS)
//   Leds    the M5GO base's 10 SK6812 LEDs
//   Clock   wall-clock days, for the focus log (Wi-Fi time, or the serial "T<epoch>" command)
//   FocusLog  focused seconds per day
//   Screen / App   the interfaces the launcher and apps implement

#pragma once
#include <Adafruit_NeoPixel.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <functional>
#include <sys/time.h>
#include <time.h>
#include <vector>
#include "gfx.h"

#define TOCK_VERSION "0.1.0"  // with the Mac app's CFBundleShortVersionString and CHANGELOG.md

// ---------- buttons ----------

enum Btn { BTN_A, BTN_B, BTN_C };
// EV_PRESS / EV_RELEASE are the raw edges, for screens that care how long a button is held.
enum BtnEv { EV_CLICK, EV_HOLD, EV_PRESS, EV_RELEASE };
constexpr uint32_t HOLD_MS = 600;

// ---------- sound ----------

inline int prefsInt(const char* key, int fallback);  // Prefs below; for the saved volume

class Sound {
 public:
  bool hushed = false;  // set by the runtime while a quiet screen is up
  bool muted = false;   // SOUND at OFF in the Settings app

  // The Settings app's SOUND: 0 = off, 1-5 = quiet to loud (3 is the old fixed level).
  static constexpr int VOLUME_LEVELS = 6;
  static int volumeLevel() {
    // before this setting there was only a mute switch; it carries over
    return constrain(prefsInt("sys.volume", prefsInt("sys.mute", 0) ? 0 : 3), 0, VOLUME_LEVELS - 1);
  }
  void setVolume(int level) {
    static const uint8_t MASTER[VOLUME_LEVELS] = {0, 24, 50, 90, 150, 230};
    muted = level == 0;
    M5.Speaker.setVolume(MASTER[level]);
  }

  void tone(uint16_t freq, uint16_t ms, uint16_t delayMs = 0) {
    if (hushed || muted) return;
    if (delayMs == 0) {
      M5.Speaker.tone(freq, ms);
      return;
    }
    for (Note& n : queue)
      if (!n.freq) {
        n = {millis() + delayMs, freq, ms};
        return;
      }
  }

  void chirp() {
    tone(1800, 50);
    tone(2600, 70, 55);
  }

  void jingle() {
    const uint16_t notes[] = {523, 659, 784, 1047};
    for (int i = 0; i < 4; i++) tone(notes[i], 90, i * 110);
  }

  void update(uint32_t now) {
    for (Note& n : queue)
      if (n.freq && (int32_t)(now - n.at) >= 0) {
        if (!hushed && !muted) M5.Speaker.tone(n.freq, n.ms);
        n.freq = 0;
      }
  }

 private:
  struct Note {
    uint32_t at;
    uint16_t freq, ms;
  };
  Note queue[8] = {};
};

// ---------- prefs ----------

class Prefs {
 public:
  void begin() { p.begin("tock", false); }
  int get(const char* key, int fallback) { return p.getInt(key, fallback); }
  void set(const char* key, int v) {
    if (p.getInt(key, v + 1) != v) p.putInt(key, v);
  }
  Preferences& raw() { return p; }

 private:
  Preferences p;
};


// ---------- LEDs ----------
// Two bars of five; here 0-4 is one bar and 5-9 the other, each bottom to top.
// The real wiring order is still to be checked on the FIRE.

constexpr int LEDS_PER_BAR = 5, LED_COUNT = 10, LED_PIN = 15;
constexpr float LED_MAX = 0.25f;  // the SK6812s are blinding at full power

class Leds {
 public:
  void begin() {
    strip.begin();
    strip.clear();
    strip.show();
  }
  void clear() {
    for (auto& p : px) p = 0;
  }
  void set(int i, uint32_t rgb, float level) {
    level = level < 0 ? 0 : level > 1 ? 1 : level;
    const float k = level * LED_MAX;
    px[i] = ((uint32_t)(((rgb >> 16) & 0xff) * k) << 16) | ((uint32_t)(((rgb >> 8) & 0xff) * k) << 8) |
            (uint32_t)((rgb & 0xff) * k);
  }
  void setBoth(int pos, uint32_t rgb, float level) {
    set(pos, rgb, level);
    set(LEDS_PER_BAR + pos, rgb, level);
  }
  // Push to the strip, only when something changed.
  void show() {
    bool changed = false;
    for (int i = 0; i < LED_COUNT; i++)
      if (px[i] != shown[i]) changed = true;
    if (!changed) return;
    for (int i = 0; i < LED_COUNT; i++) {
      strip.setPixelColor(i, px[i]);
      shown[i] = px[i];
    }
    strip.show();
  }

 private:
  Adafruit_NeoPixel strip{LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800};
  uint32_t px[LED_COUNT] = {};
  uint32_t shown[LED_COUNT] = {1};  // differs from px, so the first show() writes
};

// Full-saturation hue (degrees) as 0xRRGGBB.
inline uint32_t hue(float deg) {
  float h = fmodf(fmodf(deg, 360) + 360, 360) / 60;
  const uint8_t x = (uint8_t)roundf(255 * (1 - fabsf(fmodf(h, 2) - 1)));
  uint8_t r, g, b;
  if (h < 1) r = 255, g = x, b = 0;
  else if (h < 2) r = x, g = 255, b = 0;
  else if (h < 3) r = 0, g = 255, b = x;
  else if (h < 4) r = 0, g = x, b = 255;
  else if (h < 5) r = x, g = 0, b = 255;
  else r = 255, g = 0, b = x;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ---------- clock and calendar days ----------
// The FIRE has no battery-backed clock, so the date comes from Wi-Fi (secrets.h) or serial.

namespace clockd {

inline bool known() { return time(nullptr) > 1700000000; }

// Days since 1970-01-01 for a civil date (Howard Hinnant's days_from_civil).
inline int32_t fromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

inline void toCivil(int32_t z, int& y, int& m, int& d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

// Today as a local calendar day number, or -1 while the clock is unknown.
inline int32_t today() {
  if (!known()) return -1;
  const time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  return fromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

inline int weekday(int32_t day) { return (int)(((day % 7) + 7 + 3) % 7); }  // Monday = 0 (1970-01-01 was a Thursday)
inline int32_t weekStart(int32_t day) { return day - weekday(day); }

inline void setEpoch(time_t epoch) {
  struct timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
  if (M5.Rtc.isEnabled()) M5.Rtc.setDateTime(gmtime(&epoch));
}

}  // namespace clockd

static const char* const DAY_NAMES[] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
static const char* const MONTH_NAMES[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

// ---------- focus log ----------
// Focused seconds and finished sessions per day, kept for 400 days as an NVS blob.
// Time focused while the clock is unknown waits in a "pending" bucket, and joins today's
// total as soon as the clock is set.
// Per task (tasks.h, ids 1-8), a second blob keeps the last 120 days in 10-second steps; time
// without a task is simply the part of the day's total no task claims.

struct DayStat {
  int32_t day;
  float secs;
  uint16_t sessions;
};

constexpr int MAX_TASKS = 8;

struct TaskDay {
  int32_t day;
  uint16_t tens[MAX_TASKS];  // focused time per task id-1, in 10 s steps
};

class FocusLog {
 public:
  static constexpr int KEEP_DAYS = 400;
  static constexpr uint32_t FLUSH_MS = 15000;

  void begin(Preferences* p) {
    prefs = p;
    const size_t len = prefs->getBytesLength("focuslog");
    days.resize(len / sizeof(DayStat));
    if (len) prefs->getBytes("focuslog", days.data(), len);
    pending.secs = prefs->getFloat("pend.secs", 0);
    pending.sessions = prefs->getUShort("pend.sess", 0);
    const size_t tlen = prefs->getBytesLength("tasklog");
    taskDays.resize(tlen / sizeof(TaskDay));
    if (tlen) prefs->getBytes("tasklog", taskDays.data(), tlen);
  }

  // `task` (1-8) files the time under that task too; 0 = no task.
  void addFocus(float ms, uint32_t now, int task = 0) {
    if (ms <= 0) return;
    bucket().secs += ms / 1000;
    if (task >= 1 && task <= MAX_TASKS) {
      float& acc = taskAcc[task - 1];
      acc += ms / 1000;
      const int32_t t = clockd::today();
      if (acc >= 10 && t >= 0) {  // while the clock is unknown the seconds wait here
        const int steps = (int)(acc / 10);
        uint16_t& v = taskAt(t).tens[task - 1];
        v = min(65535, v + steps);
        acc -= steps * 10;
      }
    }
    dirty = true;
    if (now - lastFlush > FLUSH_MS) flush(now);
  }

  float taskSecs(int32_t day, int task) const {
    if (task < 1 || task > MAX_TASKS) return 0;
    for (const TaskDay& d : taskDays)
      if (d.day == day) return d.tens[task - 1] * 10.0f;
    return 0;
  }

  float taskTotal(int32_t first, int32_t last, int task) const {
    float s = 0;
    for (int32_t d = first; d <= last; d++) s += taskSecs(d, task);
    return s;
  }

  // A deleted task's time stays in each day's total, untagged. `ids` is a bit mask of task ids.
  void forgetTasks(uint16_t ids) {
    for (TaskDay& d : taskDays)
      for (int i = 0; i < MAX_TASKS; i++)
        if (ids & (1 << (i + 1))) d.tens[i] = 0;
    for (int i = 0; i < MAX_TASKS; i++)
      if (ids & (1 << (i + 1))) taskAcc[i] = 0;
    dirty = true;
    flush(millis());
  }

  void addSession(uint32_t now) {
    bucket().sessions++;
    dirty = true;
    flush(now);
  }

  DayStat get(int32_t day) const {
    for (const DayStat& d : days)
      if (d.day == day) return d;
    return {day, 0, 0};
  }

  float total(int32_t first, int32_t last) const {
    float s = 0;
    for (const DayStat& d : days)
      if (d.day >= first && d.day <= last) s += d.secs;
    return s;
  }

  void flush(uint32_t now) {
    lastFlush = now;
    mergePending();
    if (!dirty) return;
    dirty = false;
    const int32_t t = clockd::today();
    if (t >= 0) {
      std::vector<DayStat> kept;
      for (const DayStat& d : days)
        if (d.day >= t - KEEP_DAYS) kept.push_back(d);
      days.swap(kept);
    }
    prefs->putBytes("focuslog", days.data(), days.size() * sizeof(DayStat));
    if (t >= 0) {
      std::vector<TaskDay> keptTasks;
      for (const TaskDay& d : taskDays)
        if (d.day > t - TASK_DAYS) keptTasks.push_back(d);
      taskDays.swap(keptTasks);
    }
    prefs->putBytes("tasklog", taskDays.data(), taskDays.size() * sizeof(TaskDay));
    prefs->putFloat("pend.secs", pending.secs);
    prefs->putUShort("pend.sess", pending.sessions);
  }

  // Serial debug only: plausible history for looking at the Stats app, and wiping it.
  // Sessions go to the task ids in the `tasks` mask at random (some untagged).
  void seedDemo(uint16_t tasks = 0, int n = 180) {
    const int32_t t = clockd::today();
    if (t < 0) return;
    int ids[MAX_TASKS], nIds = 0;
    for (int i = 1; i <= MAX_TASKS; i++)
      if (tasks & (1 << i)) ids[nIds++] = i;
    for (int32_t d = t - n; d < t; d++) {
      const bool weekend = clockd::weekday(d) >= 5;
      if (random(100) < (weekend ? 60 : 15)) continue;
      const int sessions = 1 + random(weekend ? 3 : 9);
      at(d).secs = 0;
      at(d).sessions = sessions;
      if (d > t - TASK_DAYS) taskAt(d) = {d, {}};
      for (int i = 0; i < sessions; i++) {
        const float secs = 25 * 60 * (0.6f + random(40) / 100.0f);
        at(d).secs += secs;
        if (nIds && d > t - TASK_DAYS && random(100) < 85) taskAt(d).tens[ids[random(nIds)] - 1] += (uint16_t)(secs / 10);
      }
    }
    dirty = true;
    flush(millis());
  }
  void clear() {
    days.clear();
    taskDays.clear();
    pending = {};
    dirty = true;
    flush(millis());
  }

 private:
  Preferences* prefs = nullptr;
  static constexpr int TASK_DAYS = 120;
  std::vector<DayStat> days;
  std::vector<TaskDay> taskDays;
  float taskAcc[MAX_TASKS] = {};  // seconds not yet a whole 10 s step
  DayStat pending = {-1, 0, 0};
  bool dirty = false;
  uint32_t lastFlush = 0;

  DayStat& at(int32_t day) {
    for (DayStat& d : days)
      if (d.day == day) return d;
    days.push_back({day, 0, 0});
    return days.back();
  }

  TaskDay& taskAt(int32_t day) {
    for (TaskDay& d : taskDays)
      if (d.day == day) return d;
    taskDays.push_back({day, {}});
    return taskDays.back();
  }

  DayStat& bucket() {
    const int32_t t = clockd::today();
    return t < 0 ? pending : at(t);
  }

  void mergePending() {
    if ((pending.secs <= 0 && !pending.sessions) || !clockd::known()) return;
    DayStat& d = at(clockd::today());
    d.secs += pending.secs;
    d.sessions += pending.sessions;
    pending = {-1, 0, 0};
    dirty = true;
  }
};

inline void formatDuration(char* out, size_t n, float secs) {
  const int mins = (int)(secs / 60);
  const int h = mins / 60, m = mins % 60;
  if (h == 0) snprintf(out, n, "%dM", m);
  else if (m == 0) snprintf(out, n, "%dH", h);
  else snprintf(out, n, "%dH %dM", h, m);
}

// ---------- settings, screens, apps ----------

struct Setting {
  const char* label;
  const char* const* options;
  int count;
  std::function<int()> index;
  std::function<void(int)> choose;
};
using Settings = std::vector<Setting>;

class Screen {
 public:
  virtual ~Screen() {}
  virtual void enter(uint32_t now) {}
  virtual void leave(uint32_t now) {}
  virtual void update(uint32_t now, float dt) {}
  virtual void draw(Gfx& g, uint32_t now) = 0;
  virtual void button(Btn b, BtnEv ev, uint32_t now) {}
  virtual void drawLeds(Leds& leds, uint32_t now) {}
  virtual Settings settings() { return {}; }
  virtual bool quiet() { return false; }  // a quiet screen makes no sound at all
};

constexpr int ICON_SIZE = 56;

class App : public Screen {
 public:
  virtual const char* name() = 0;
  virtual void drawIcon(Gfx& g, int x, int y, uint32_t now, bool live) = 0;
  virtual void subtitle(char* out, size_t n) = 0;
  virtual bool badge() { return false; }
};

// The things apps reach for, owned by tock.ino.
struct Sys {
  Sound sound;
  Prefs prefs;
  Leds leds;
  FocusLog focus;
  float timeScale = 1;  // serial debug: countdowns run faster
};
extern Sys sys;

inline int prefsInt(const char* key, int fallback) { return sys.prefs.get(key, fallback); }

// What the Timer is doing, for the Mac's menu bar (net.h); the Timer app keeps it current.
struct TimerStatus {
  const char* state = "ready";  // ready, running, paused, done, waiting (a break is over)
  bool onBreak = false;
  uint32_t leftMs = 0, totalMs = 0;
  int round = 1, rounds = 1, task = 0;
};
inline TimerStatus timerStatus;

// The daily goal, in minutes (any length: the Mac app can set one that isn't a preset), and the
// day it was last set (the Mac app reminds you to set one each new day). Older firmware kept an
// index into GOAL_HOURS in "stats.goal"; that still counts until a goal is set in minutes.
static const int GOAL_HOURS[7] = {1, 2, 3, 4, 5, 6, 8};
constexpr int GOAL_MIN_MINUTES = 15, GOAL_MAX_MINUTES = 16 * 60;
inline int goalMinutes() {
  const int m = sys.prefs.get("stats.goalmin", 0);
  return m > 0 ? m : GOAL_HOURS[constrain(sys.prefs.get("stats.goal", 3), 0, 6)] * 60;
}
inline void setGoalMinutes(int minutes) {
  sys.prefs.set("stats.goalmin", constrain(minutes, GOAL_MIN_MINUTES, GOAL_MAX_MINUTES));
  sys.prefs.set("stats.goalday", clockd::today());
}
