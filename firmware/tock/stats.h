// stats.h: the Stats app. Mirrors simulator/src/apps/stats.
//   A  day before     B  next view (DAY, WEEK, TASKS, HEATMAP)     C  day after
//   hold C: settings (daily goal)
// TASKS (that week per task) only shows once there are tasks; with tasks, the day and week bars
// are split in the tasks' colors, untagged time keeping mustard (dim on unselected days).
// Needs the wall clock for calendar days; until it is set, it says so.

#pragma once
#include "sprites.h"
#include "system.h"
#include "tasks.h"

static const char* const GOAL_PRESETS[7] = {"1 H", "2 H", "3 H", "4 H", "5 H", "6 H", "8 H"};

class StatsApp : public App {
 public:
  StatsApp() {
    view = constrain(sys.prefs.get("stats.view", 0), 0, V_COUNT - 1);
  }

  const char* name() override { return "STATS"; }

  void subtitle(char* out, size_t n) override {
    if (!clockd::known()) return (void)snprintf(out, n, "NO CLOCK YET");
    char d[16];
    formatDuration(d, sizeof d, sys.focus.get(clockd::today()).secs);
    snprintf(out, n, "%s TODAY", d);
  }

  // A dark tile with a little heatmap; when selected, today's square glows.
  void drawIcon(Gfx& g, int x, int y, uint32_t now, bool live) override {
    static const int PATTERN[4][4] = {{0, 1, 2, 1}, {1, 3, 2, 4}, {2, 4, 3, 2}, {1, 2, 4, 3}};
    constexpr int CELL = 9, GAP = 3, SPAN = 4 * CELL + 3 * GAP;
    g.fillRect(x, y, ICON_SIZE, ICON_SIZE, rgb565(0x26231f));
    const int x0 = x + (ICON_SIZE - SPAN) / 2, y0 = y + (ICON_SIZE - SPAN) / 2;
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) {
        int level = PATTERN[r][c];
        if (r == 3 && c == 3 && live) level = (now / 500) % 2 ? 4 : 2;
        g.fillRect(x0 + c * (CELL + GAP), y0 + r * (CELL + GAP), CELL, CELL, heat(level));
      }
  }

  Settings settings() override {
    // the presets, plus the current goal when it's one the Mac set that isn't a preset
    static const char* opts[8];
    static char custom[12];
    int n = 7, cur = -1;
    for (int i = 0; i < 7; i++) {
      opts[i] = GOAL_PRESETS[i];
      if (GOAL_HOURS[i] * 60 == goalMinutes()) cur = i;
    }
    if (cur < 0) {
      formatDuration(custom, sizeof custom, goalMinutes() * 60.0f);
      opts[7] = custom;
      cur = n++;
    }
    return {{"DAILY GOAL", opts, n, [cur] { return cur; }, [](int i) {
               if (i < 7) setGoalMinutes(GOAL_HOURS[i] * 60);
             }}};
  }

  void enter(uint32_t now) override {
    day = clockd::today();
    if (view == V_TASKS && !tasks.count()) view = V_DAY;
  }

  void button(Btn b, BtnEv ev, uint32_t now) override {
    if (ev != EV_CLICK) return;
    if (b == BTN_B) {
      view = nextView();
      sys.prefs.set("stats.view", view);
      sys.sound.tone(1600, 30);
      return;
    }
    const int32_t today = clockd::today();
    if (today < 0) return;
    if (day < 0) day = today;
    const int32_t next = day + (b == BTN_A ? -1 : 1);
    if (next > today || next < today - FocusLog::KEEP_DAYS + 1) {
      sys.sound.tone(300, 40);
      return;
    }
    day = next;
    movedAt = now;
    sys.sound.tone(1500, 20);
  }

  void draw(Gfx& g, uint32_t now) override {
    g.fillScreen(pal::bg);
    const int32_t today = clockd::today();
    if (today < 0) return drawNoClock(g, now);
    if (day < 0 || day > today) day = today;
    if (view == V_TASKS && !tasks.count()) view = V_DAY;
    if (view == V_DAY) drawDay(g, now, today);
    else if (view == V_WEEK) drawWeek(g, today);
    else if (view == V_TASKS) drawTasks(g, today);
    else drawHeatmap(g, now, today);
    // which view this is: one mark per view shown
    const int n = tasks.count() ? V_COUNT : V_COUNT - 1;
    const int pos = !tasks.count() && view == V_HEATMAP ? view - 1 : view;
    for (int k = 0; k < n; k++) g.fillRect(SCREEN_W / 2 - n * 6 + 2 + k * 12, 224, 8, 4, k == pos ? pal::ink : pal::faint);
  }

 private:
  enum View { V_DAY, V_WEEK, V_TASKS, V_HEATMAP, V_COUNT };
  static constexpr int WEEKS = 16;

  int nextView() const {
    int v = (view + 1) % V_COUNT;
    if (v == V_TASKS && !tasks.count()) v++;
    return v;
  }

  // A bar `len` px long for `total` seconds, stacked from its start in task order: each task in
  // its color, untagged time last in mustard (dim unless `lit`). Vertical bars grow up from y.
  void drawSplit(Gfx& g, int x, int y, int len, int thick, int32_t d, float secs, float total, bool lit, bool vertical) {
    const int full = (int)roundf(len * secs / total);
    int done = 0;
    float used = 0;
    auto seg = [&](float s, uint16_t color) {
      if (s <= 0) return;
      used += s;
      const int end = min(full, (int)roundf(full * used / secs));
      if (end > done) {
        if (vertical) g.fillRect(x, y - end, thick, end - done, color);
        else g.fillRect(x + done, y, end - done, thick, color);
      }
      done = end;
    };
    for (int i = 0; i < tasks.count(); i++) seg(sys.focus.taskSecs(d, tasks.at(i).id), tasks.color(tasks.at(i).id, pal::body));
    seg(secs - used, lit ? pal::body : pal::dim);
  }

  void drawTasks(Gfx& g, int32_t today) {
    const int32_t ws = clockd::weekStart(day), we = min(ws + 6, today);
    const float total = sys.focus.total(ws, we);
    struct Row {
      const char* name;
      uint16_t color;
      float secs;
    } rows[MAX_TASKS + 1];
    int n = 0;
    float tagged = 0, most = 1;
    for (int i = 0; i < tasks.count(); i++) {
      const Task& t = tasks.at(i);
      rows[n++] = {t.name, tasks.color(t.id, pal::body), sys.focus.taskTotal(ws, we, t.id)};
      tagged += rows[n - 1].secs;
    }
    if (total - tagged >= 60) rows[n++] = {"UNTAGGED", pal::dim, total - tagged};

    char buf[24], date[20];
    dateLabel(date, sizeof date, ws);
    snprintf(buf, sizeof buf, "TASKS %s", date + 4);
    g.drawText(buf, 16, 14, 2, pal::ink);
    formatDuration(buf, sizeof buf, total);
    g.drawText(buf, SCREEN_W - 16, 14, 2, pal::body, RIGHT);

    int nameW = 0;
    for (int i = 0; i < n; i++) {
      most = max(most, rows[i].secs);
      nameW = max(nameW, g.textWidth(rows[i].name, 2));
    }
    const int rowH = min(22, 172 / max(1, n));
    const int barX = 30 + nameW + 10, barMax = SCREEN_W - 16 - 64 - barX;
    for (int i = 0; i < n; i++) {
      const int y = 44 + i * rowH;
      const bool some = rows[i].secs > 0;
      g.fillRect(16, y + 1, 8, 8, rows[i].color);
      g.drawText(rows[i].name, 30, y, 2, some ? pal::ink : pal::grey);
      g.fillRect(barX, y + 1, max(2, (int)roundf(barMax * rows[i].secs / most)), 8, some ? rows[i].color : pal::faint);
      if (some) formatDuration(buf, sizeof buf, rows[i].secs);
      g.drawText(some ? buf : "-", SCREEN_W - 16, y, 2, some ? pal::ink : pal::grey, RIGHT);
    }
    if (total <= 0) g.drawText("NOTHING THIS WEEK YET", SCREEN_W / 2, 200, 2, pal::grey, CENTER);
  }
  int view = 0;
  int32_t day = -1;
  uint32_t movedAt = 0;

  float goalSecs() const { return goalMinutes() * 60.0f; }  // read each time: the Mac can change it

  // read at draw time: the colors follow the theme
  static uint16_t heat(int level) {
    const uint16_t h[5] = {pal::faint, pal::umber, pal::dim, pal::amber, pal::body};
    return h[level];
  }

  static int heatLevel(float s) { return s <= 0 ? 0 : s < 3600 ? 1 : s < 7200 ? 2 : s < 14400 ? 3 : 4; }

  static void dateLabel(char* out, size_t n, int32_t d) {
    int y, m, dd;
    clockd::toCivil(d, y, m, dd);
    snprintf(out, n, "%s %d %s", DAY_NAMES[clockd::weekday(d)], dd, MONTH_NAMES[m - 1]);
  }

  void drawNoClock(Gfx& g, uint32_t now) {
    g.drawText("NO CLOCK YET", SCREEN_W / 2, 70, 3, pal::ink, CENTER);
    g.drawText("CONNECT WI-FI", SCREEN_W / 2, 110, 2, pal::grey, CENTER);
    g.drawText("OR THE MAC APP", SCREEN_W / 2, 130, 2, pal::grey, CENTER);
    drawTock(g, still(POSE_SIT), (SCREEN_W - TOCK_W * 4) / 2, 210, 4);
  }

  void drawDay(Gfx& g, uint32_t now, int32_t today) {
    const DayStat st = sys.focus.get(day);
    char buf[24];

    if (day == today) g.drawText("TODAY", 16, 14, 2, pal::ink);
    else if (day == today - 1) g.drawText("YESTERDAY", 16, 14, 2, pal::ink);
    else {
      dateLabel(buf, sizeof buf, day);
      g.drawText(buf, 16, 14, 2, pal::ink);
    }
    if (day >= today - 1) {
      dateLabel(buf, sizeof buf, day);
      g.drawText(buf, SCREEN_W - 16, 14, 2, pal::grey, RIGHT);
    }

    formatDuration(buf, sizeof buf, st.secs);
    g.drawText(buf, 16, 50, 6, st.secs > 0 ? pal::body : pal::grey);
    snprintf(buf, sizeof buf, st.sessions == 1 ? "1 SESSION" : "%u SESSIONS", st.sessions);
    g.drawText(buf, 16, 92, 2, pal::grey);
    if (tasks.count() && st.secs >= 60) drawSplit(g, 16, 112, 200, 8, day, st.secs, st.secs, true, false);

    Act a = idleAct(now);
    if (st.secs <= 0) a = still(POSE_SIT);
    else if (st.secs >= goalSecs()) {
      const uint32_t k = now % 2400;
      a = still(POSE_CHEER, k < 700 ? HOP[(k / 117) % HOP_N] : 0);
    }
    drawTock(g, a, SCREEN_W - 16 - TOCK_W * 4, 118, 4);

    g.fillRect(16, 136, SCREEN_W - 32, 2, pal::faint);
    const int32_t ws = clockd::weekStart(day);
    int y, m, dd;
    clockd::toCivil(day, y, m, dd);
    const int32_t monthStart = day - (dd - 1);
    const int32_t monthEnd = clockd::fromCivil(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - 1;
    drawTotal(g, 16, 152, "WEEK", sys.focus.total(ws, min(ws + 6, today)));
    drawTotal(g, 168, 152, MONTH_NAMES[m - 1], sys.focus.total(monthStart, min(monthEnd, today)));
  }

  void drawTotal(Gfx& g, int x, int y, const char* label, float secs) {
    char buf[16];
    g.drawText(label, x, y, 2, pal::grey);
    formatDuration(buf, sizeof buf, secs);
    g.drawText(buf, x, y + 18, 3, pal::ink);
  }

  void drawWeek(Gfx& g, int32_t today) {
    const int32_t ws = clockd::weekStart(day);
    float secs[7], sum = 0, maxS = goalSecs();
    for (int i = 0; i < 7; i++) {
      secs[i] = ws + i <= today ? sys.focus.get(ws + i).secs : 0;
      sum += secs[i];
      maxS = max(maxS, secs[i]);
    }
    char buf[24], date[20];
    dateLabel(date, sizeof date, ws);
    snprintf(buf, sizeof buf, "WEEK OF %s", date + 4);
    g.drawText(buf, 16, 14, 2, pal::ink);
    formatDuration(buf, sizeof buf, sum);
    g.drawText(buf, SCREEN_W - 16, 14, 2, pal::body, RIGHT);

    constexpr int TOP = 58, BOTTOM = 184, BAR_W = 28, GAP = 12;
    const int x0 = (SCREEN_W - (7 * BAR_W + 6 * GAP)) / 2;
    for (int h = 2; h * 3600 <= maxS; h += 2) {
      const int y = BOTTOM - (int)roundf((BOTTOM - TOP) * h * 3600 / maxS);
      for (int x = x0; x < x0 + 7 * BAR_W + 6 * GAP; x += 6) g.fillRect(x, y, 2, 2, pal::faint);
    }
    for (int i = 0; i < 7; i++) {
      const int x = x0 + i * (BAR_W + GAP);
      const int h = max(2, (int)roundf((BOTTOM - TOP) * secs[i] / maxS));
      const bool selected = ws + i == day, future = ws + i > today;
      if (secs[i] > 0 && tasks.count()) drawSplit(g, x, BOTTOM, BOTTOM - TOP, BAR_W, ws + i, secs[i], maxS, selected, true);
      else g.fillRect(x, BOTTOM - h, BAR_W, h, future ? pal::bg : secs[i] > 0 ? (selected ? pal::body : pal::dim) : pal::faint);
      const char letter[2] = {DAY_NAMES[i][0], 0};
      g.drawText(letter, x + BAR_W / 2, BOTTOM + 8, 2, selected ? pal::ink : pal::grey, CENTER);
      if (selected) g.fillRect(x, BOTTOM + 22, BAR_W, 2, pal::ink);
    }
    const int sel = day - ws;
    formatDuration(buf, sizeof buf, secs[sel]);
    const int half = g.textWidth(buf, 2) / 2;
    const int lx = constrain(x0 + sel * (BAR_W + GAP) + BAR_W / 2, 8 + half, SCREEN_W - 8 - half);
    const int barTop = BOTTOM - max(2, (int)roundf((BOTTOM - TOP) * secs[sel] / maxS));
    g.drawText(buf, lx, barTop - 16, 2, pal::ink, CENTER);
  }

  void drawHeatmap(Gfx& g, uint32_t now, int32_t today) {
    int32_t lastWeek = clockd::weekStart(today);
    if (day < lastWeek - (WEEKS - 1) * 7) lastWeek = clockd::weekStart(day) + (WEEKS - 1) * 7;
    const int32_t first = lastWeek - (WEEKS - 1) * 7;
    constexpr int CELL = 13, GAP = 3, PITCH = CELL + GAP, X0 = 34, Y0 = 52;
    char buf[24];

    g.drawText("FOCUS", 16, 14, 2, pal::ink);
    g.drawText("16 WEEKS", SCREEN_W - 16, 14, 2, pal::grey, RIGHT);
    for (int r : {0, 2, 4}) {
      const char letter[2] = {DAY_NAMES[r][0], 0};
      g.drawText(letter, 16, Y0 + r * PITCH + 2, 2, pal::grey);
    }

    auto monthStartsIn = [&](int c) {
      for (int r = 0; r < 7; r++) {
        int y, m, d;
        clockd::toCivil(first + c * 7 + r, y, m, d);
        if (d == 1) return m - 1;
      }
      return -1;
    };
    for (int c = 0; c < WEEKS - 1; c++) {
      int month = monthStartsIn(c);
      if (c == 0 && month < 0 && monthStartsIn(1) < 0 && monthStartsIn(2) < 0) {
        int y, m, d;
        clockd::toCivil(first, y, m, d);
        month = m - 1;
      }
      if (month >= 0) g.drawText(MONTH_NAMES[month], X0 + c * PITCH, 34, 2, pal::grey);
    }

    for (int c = 0; c < WEEKS; c++)
      for (int r = 0; r < 7; r++) {
        const int32_t d = first + c * 7 + r;
        if (d > today) continue;
        g.fillRect(X0 + c * PITCH, Y0 + r * PITCH, CELL, CELL, heat(heatLevel(sys.focus.get(d).secs)));
      }

    const int selC = (clockd::weekStart(day) - first) / 7, selR = clockd::weekday(day);
    if (((now - movedAt) / 600) % 2 == 0 || now - movedAt < 600) {
      const int x = X0 + selC * PITCH - 2, y = Y0 + selR * PITCH - 2;
      g.fillRect(x, y, CELL + 4, 2, pal::ink);
      g.fillRect(x, y + CELL + 2, CELL + 4, 2, pal::ink);
      g.fillRect(x, y, 2, CELL + 4, pal::ink);
      g.fillRect(x + CELL + 2, y, 2, CELL + 4, pal::ink);
    }

    constexpr int SQ = 10, SGAP = 3;
    const int ly = Y0 + 7 * PITCH + 6;
    const int sqX = SCREEN_W - 16 - g.textWidth("MORE", 2) - 6 - (5 * (SQ + SGAP) - SGAP);
    g.drawText("LESS", sqX - 6, ly, 2, pal::grey, RIGHT);
    for (int i = 0; i < 5; i++) g.fillRect(sqX + i * (SQ + SGAP), ly, SQ, SQ, heat(i));
    g.drawText("MORE", SCREEN_W - 16, ly, 2, pal::grey, RIGHT);

    dateLabel(buf, sizeof buf, day);
    g.drawText(buf, 16, 196, 2, pal::grey);
    formatDuration(buf, sizeof buf, sys.focus.get(day).secs);
    g.drawText(buf, SCREEN_W - 16, 196, 2, pal::ink, RIGHT);
  }
};
