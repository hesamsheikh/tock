// tasks.h: the user's own lists, made in the Mac app and sent over Bluetooth (net.h).
// Mirrors simulator/src/os/tasks.ts.
//   Tasks     groups to file focus time under (STUDY, WORK, ...), each with a color. Optional:
//             time without a task is simply untagged. Up to 8, ids 1-8 (0 = no task).
//             Stored as lines "id|color|name" in NVS "tasks"; the current one in "task.cur".
//   Messages  the screensaver's lines, stored as lines in NVS "saver.msgs".

#pragma once
#include "system.h"

// Task colors: muted, warm-leaning tones that sit with Tock's mustard in both themes.
// Mustard itself stays for untagged time.
static const uint32_t TASK_RGB[] = {0xe9785b, 0x3fb8a8, 0x9bbf6a, 0x5fa8e0, 0xa98be0, 0xe07fa8, 0xcdb88f, 0x8a9bb0};
constexpr int TASK_COLOR_COUNT = 8, TASK_NAME_MAX = 12, MAX_MESSAGES = 16, MESSAGE_MAX = 28;

// Only what the 3 x 5 pixel font can draw, upper case, single spaces, at most `max` letters.
inline String cleanText(const String& in, int max) {
  String out;
  bool space = false;
  for (size_t i = 0; i < in.length() && (int)out.length() < max; i++) {
    char c = toupper(in[i]);
    if (c == ' ' || c == '\t') {
      space = out.length() > 0;
      continue;
    }
    if (!isalnum(c) && !strchr(":!.?-/%+,'", c)) continue;
    if (space && (int)out.length() < max - 1) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

struct Task {
  int id = 0;  // 1-8, stable: focus time is filed under it
  int color = 0;
  char name[TASK_NAME_MAX + 1] = "";
};

class Tasks {
 public:
  void begin() {
    parse(sys.prefs.raw().getString("tasks", ""));
    cur = get(sys.prefs.get("task.cur", 0)) ? sys.prefs.get("task.cur", 0) : 0;
  }

  int count() const { return n; }
  const Task& at(int i) const { return items[i]; }
  const Task* get(int id) const {
    for (int i = 0; i < n; i++)
      if (items[i].id == id) return &items[i];
    return nullptr;
  }
  uint16_t color(int id, uint16_t fallback) const {
    const Task* t = get(id);
    return t ? rgb565(TASK_RGB[t->color % TASK_COLOR_COUNT]) : fallback;
  }
  uint32_t rgb(int id, uint32_t fallback) const {  // for the LEDs
    const Task* t = get(id);
    return t ? TASK_RGB[t->color % TASK_COLOR_COUNT] : fallback;
  }
    // The ids in use, as a bit mask (bit id).
  uint16_t mask() const {
    uint16_t m = 0;
    for (int i = 0; i < n; i++) m |= 1 << items[i].id;
    return m;
  }

  // The task the Timer files new focus under (0 = none), remembered across restarts.
  int current() const { return cur; }
  void setCurrent(int id) {
    cur = get(id) ? id : 0;
    sys.prefs.set("task.cur", cur);
  }
  // Next in the cycle none -> first -> ... -> last -> none.
  int next(int id) const {
    for (int i = 0; i < n; i++)
      if (items[i].id == id) return i + 1 < n ? items[i + 1].id : 0;
    return n ? items[0].id : 0;
  }

  // For the Timer's settings sheet: "NONE" and then the names, in order.
  const char* const* optionNames() {
    opts[0] = "NONE";
    for (int i = 0; i < n; i++) opts[i + 1] = items[i].name;
    return opts;
  }

  // The whole list from the Mac app: lines "id|color|name" (id 0 = new: gets the lowest free
  // id). Tasks that disappear take their per-task time with them (it stays in the day totals).
  void setAll(const String& text) {
    const uint16_t before = mask();
    parse(text);
    String out;
    for (int i = 0; i < n; i++) out += String(items[i].id) + "|" + items[i].color + "|" + items[i].name + "\n";
    sys.prefs.raw().putString("tasks", out);
    if (!get(cur)) setCurrent(0);
    const uint16_t gone = before & ~mask();
    if (gone) sys.focus.forgetTasks(gone);
    version++;
  }

  // Lines "id|color|name", as stored (for Bluetooth).
  String text() const { return sys.prefs.raw().getString("tasks", ""); }

  uint32_t version = 0;  // bumped on every change

 private:
  Task items[MAX_TASKS];
  int n = 0, cur = 0;
  const char* opts[MAX_TASKS + 1] = {};

  void parse(const String& text) {
    n = 0;
    uint16_t used = 0;
    Task fresh[MAX_TASKS];
    int nFresh = 0;
    int start = 0;
    while (start < (int)text.length() && n + nFresh < MAX_TASKS) {
      int end = text.indexOf('\n', start);
      if (end < 0) end = text.length();
      const String line = text.substring(start, end);
      start = end + 1;
      const int a = line.indexOf('|'), b = line.indexOf('|', a + 1);
      if (a < 0 || b < 0) continue;
      Task t;
      t.id = line.substring(0, a).toInt();
      t.color = constrain(line.substring(a + 1, b).toInt(), 0, TASK_COLOR_COUNT - 1);
      const String name = cleanText(line.substring(b + 1), TASK_NAME_MAX);
      if (!name.length()) continue;
      strlcpy(t.name, name.c_str(), sizeof t.name);
      if (t.id >= 1 && t.id <= MAX_TASKS && !(used & (1 << t.id))) {
        used |= 1 << t.id;
        items[n++] = t;
      } else {
        fresh[nFresh++] = t;
      }
    }
    for (int i = 0; i < nFresh && n < MAX_TASKS; i++) {  // new ones: the lowest free ids
      int id = 1;
      while (used & (1 << id)) id++;
      used |= 1 << id;
      fresh[i].id = id;
      items[n++] = fresh[i];
    }
  }
};

class Messages {
 public:
  void begin() { parse(sys.prefs.raw().getString("saver.msgs", "")); }

  int count() const { return n; }
  const char* at(int i) const { return items[i]; }

  // Lines from the Mac app; an empty list brings back the defaults.
  void setAll(const String& text) {
    parse(text);
    String out;
    for (int i = 0; i < n; i++) out += String(items[i]) + "\n";
    sys.prefs.raw().putString("saver.msgs", out);
    version++;
  }

  String text() const {
    String out;
    for (int i = 0; i < n; i++) out += String(items[i]) + "\n";
    return out;
  }

  uint32_t version = 0;

 private:
  char items[MAX_MESSAGES][MESSAGE_MAX + 1];
  int n = 0;

  void parse(const String& text) {
    n = 0;
    int start = 0;
    while (start < (int)text.length() && n < MAX_MESSAGES) {
      int end = text.indexOf('\n', start);
      if (end < 0) end = text.length();
      const String line = cleanText(text.substring(start, end), MESSAGE_MAX);
      start = end + 1;
      if (line.length()) strlcpy(items[n++], line.c_str(), MESSAGE_MAX + 1);
    }
    if (!n) {
      strlcpy(items[n++], "EVERY SECOND COUNTS.", MESSAGE_MAX + 1);
      strlcpy(items[n++], "YOU OWE HIM.", MESSAGE_MAX + 1);
    }
  }
};

inline Tasks tasks;
inline Messages messages;
