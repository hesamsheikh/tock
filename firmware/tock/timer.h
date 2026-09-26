// timer.h: the Timer app. Mirrors simulator/src/apps/timer (model, config, styles, break, index).
//   A  next task (pet Tock if there are no tasks)     hold A: reset
//   B  start / pause / resume
//   C  between countdowns: next time; during one: pet Tock
//   hold C: settings (task, look, mode, times, rounds)
// Tasks (tasks.h) are optional; once there are any, a label shows which one the focus counts for,
// A switches (mid-countdown too: the rest counts for the new one), and the LEDs fill in its color.
// Modes: TIMER (one countdown), POMODORO (focus, break, repeat; long break after the last round;
// breaks wait for you), INTERVAL (work and rest back to back, in seconds).

#pragma once
#include "sprites.h"
#include "system.h"
#include "tasks.h"

// ---------- model ----------

enum TState { T_READY, T_INTRO, T_RUNNING, T_PAUSED, T_DONE, T_WAITING };
enum TPhase { PH_FOCUS, PH_BREAK };
enum TMode { M_TIMER, M_POMODORO, M_INTERVAL };

struct Timer {
  TState state = T_READY;
  TPhase phase = PH_FOCUS;
  TMode mode = M_POMODORO;
  int round = 1, rounds = 4;
  bool longBreak = false;
  uint32_t total = 60000;  // ms
  float elapsed = 0;       // ms used so far
  uint32_t since = 0;      // when the state began
  uint32_t petAt = 0;
  bool petted = false;
  int task = 0;  // the task this focus counts for (0 = none)
};

constexpr uint32_t PET_MS = 1400, ALARM_CYCLE_MS = 3200, ALARM_CYCLES = 2, ALARM_RING_MS = 1200;
constexpr uint32_t ALARM_MS = ALARM_CYCLE_MS * ALARM_CYCLES;

inline uint32_t remaining(const Timer& t) { return t.elapsed >= t.total ? 0 : (uint32_t)(t.total - t.elapsed); }
inline bool petting(const Timer& t, uint32_t now) { return t.petted && now - t.petAt < PET_MS; }

// What Tock does during a focus countdown; every look draws Tock from this.
inline Act timerAct(const Timer& tm, uint32_t now) {
  const bool blink = now % 3300 < 140;
  Act a = still(blink ? POSE_BLINK : POSE_STAND);
  if (petting(tm, now)) {
    a.pose = POSE_CHEER;
    a.lift = HOP[((now - tm.petAt) / 117) % HOP_N];
    return a;
  }
  switch (tm.state) {
    case T_READY: {
      const int beat = (now / 520) % 2;
      if (!blink && beat) a.pose = POSE_SHUFFLE;
      a.lift = beat;
      break;
    }
    case T_INTRO:
    case T_RUNNING:
      a.pose = ((uint32_t)tm.elapsed / 1000) % 2 ? POSE_TOCK : POSE_TICK;
      if (now % 5200 < 160) a.pose = POSE_BLINK;
      break;
    case T_PAUSED:
      a.pose = POSE_SIT;
      break;
    case T_DONE: {
      const uint32_t el = now - tm.since;
      if (el >= ALARM_MS) break;
      const uint32_t cycle = el % ALARM_CYCLE_MS;
      if (cycle < ALARM_RING_MS) {
        a.shake = (cycle / 60) % 2 ? 1 : -1;
        a.rings = true;
      } else if (cycle < 2600) {
        a.pose = POSE_CHEER;
        a.lift = HOP[((cycle - ALARM_RING_MS) / 117) % HOP_N];
        a.sparks = a.lift > 1;
      }
      break;
    }
    default:
      break;
  }
  return a;
}

// ---------- config ----------

static const char* const LOOK_OPTS[] = {"FLUID", "ARCADE", "BAR", "BLOCKS"};
static const char* const MODE_OPTS[] = {"TIMER", "POMODORO", "INTERVAL"};
static const int FOCUS_MIN[] = {1, 5, 10, 15, 20, 25, 30, 45, 50, 60};
static const char* const FOCUS_OPTS[] = {"1 MIN", "5 MIN", "10 MIN", "15 MIN", "20 MIN", "25 MIN", "30 MIN", "45 MIN", "50 MIN", "60 MIN"};
static const int BREAK_MIN[] = {1, 3, 5, 10, 15};
static const char* const BREAK_OPTS[] = {"1 MIN", "3 MIN", "5 MIN", "10 MIN", "15 MIN"};
static const int LONG_BREAK_MIN[] = {10, 15, 20, 30};
static const char* const LONG_BREAK_OPTS[] = {"10 MIN", "15 MIN", "20 MIN", "30 MIN"};
static const int ROUNDS[] = {2, 3, 4, 5, 6, 8};
static const char* const ROUNDS_OPTS[] = {"2", "3", "4", "5", "6", "8"};
static const int WORK_SEC[] = {20, 30, 45, 60, 90, 120, 300};
static const char* const WORK_OPTS[] = {"20 SEC", "30 SEC", "45 SEC", "1 MIN", "1:30", "2 MIN", "5 MIN"};
static const int REST_SEC[] = {10, 15, 20, 30, 60, 120};
static const char* const REST_OPTS[] = {"10 SEC", "15 SEC", "20 SEC", "30 SEC", "1 MIN", "2 MIN"};

#define COUNT(a) (int)(sizeof(a) / sizeof(a[0]))

enum Field { F_LOOK, F_MODE, F_FOCUS, F_BREAK, F_LONG, F_ROUNDS, F_WORK, F_REST, F_COUNT };
struct FieldDef {
  const char* key;
  const char* const* opts;
  int count;
  int def;
};
static const FieldDef FIELDS[F_COUNT] = {
  {"timer.look", LOOK_OPTS, COUNT(LOOK_OPTS), 0},       {"timer.mode", MODE_OPTS, COUNT(MODE_OPTS), 1},
  {"timer.focus", FOCUS_OPTS, COUNT(FOCUS_OPTS), 5},    {"timer.break", BREAK_OPTS, COUNT(BREAK_OPTS), 2},
  {"timer.long", LONG_BREAK_OPTS, COUNT(LONG_BREAK_OPTS), 1},       {"timer.rounds", ROUNDS_OPTS, COUNT(ROUNDS_OPTS), 2},
  {"timer.work", WORK_OPTS, COUNT(WORK_OPTS), 2},       {"timer.rest", REST_OPTS, COUNT(REST_OPTS), 1},
};

class TimerConfig {
 public:
  void load() {
    for (int f = 0; f < F_COUNT; f++) idx[f] = constrain(sys.prefs.get(FIELDS[f].key, FIELDS[f].def), 0, FIELDS[f].count - 1);
  }
  int get(Field f) const { return idx[f]; }
  void set(Field f, int i) {
    idx[f] = i;
    sys.prefs.set(FIELDS[f].key, i);
  }
  TMode mode() const { return (TMode)idx[F_MODE]; }
  uint32_t focusMs() const { return mode() == M_INTERVAL ? WORK_SEC[idx[F_WORK]] * 1000UL : FOCUS_MIN[idx[F_FOCUS]] * 60000UL; }
  uint32_t breakMs(bool longBreak) const {
    if (mode() == M_INTERVAL) return REST_SEC[idx[F_REST]] * 1000UL;
    return (longBreak ? LONG_BREAK_MIN[idx[F_LONG]] : BREAK_MIN[idx[F_BREAK]]) * 60000UL;
  }
  int rounds() const { return mode() == M_TIMER ? 1 : ROUNDS[idx[F_ROUNDS]]; }

  // The C button: the next countdown length for this mode (wrapping around); returns its label.
  const char* nextTime() {
    const Field f = mode() == M_INTERVAL ? F_WORK : F_FOCUS;
    set(f, (idx[f] + 1) % FIELDS[f].count);
    return FIELDS[f].opts[idx[f]];
  }

  Settings settings(std::function<void(Field)> onChange) {
    Settings rows;
    auto line = [&](Field f, const char* label) {
      rows.push_back({label, FIELDS[f].opts, FIELDS[f].count, [this, f] { return idx[f]; },
                      [this, f, onChange](int i) { set(f, i); onChange(f); }});
    };
    line(F_LOOK, "LOOK");
    line(F_MODE, "MODE");
    if (mode() == M_TIMER) line(F_FOCUS, "TIME");
    if (mode() == M_POMODORO) {
      line(F_FOCUS, "FOCUS");
      line(F_BREAK, "BREAK");
      line(F_LONG, "LONG BREAK");
      line(F_ROUNDS, "ROUNDS");
    }
    if (mode() == M_INTERVAL) {
      line(F_WORK, "WORK");
      line(F_REST, "REST");
      line(F_ROUNDS, "ROUNDS");
    }
    return rows;
  }

 private:
  int idx[F_COUNT] = {};
};

// ---------- looks ----------

struct Style {
  const char* name;
  uint32_t introMs;
  void (*draw)(Gfx&, const Timer&, uint32_t);
  int tagY;  // where the task label goes (at x 12)
};

// Fluid: Tock, big, and the time. Mustard fills the screen and drains with the time;
// underwater, Tock and the time take the background color.
inline void drawFluid(Gfx& g, const Timer& tm, uint32_t now) {
  constexpr int CELL = 4, PX = 12, COLS = SCREEN_W / CELL;
  constexpr uint32_t INTRO = 1600;
  g.fillScreen(pal::bg);

  float level = 0;
  if (tm.state == T_INTRO) level = easeOut((now - tm.since) / (float)INTRO);
  else if (tm.state == T_RUNNING || tm.state == T_PAUSED) level = remaining(tm) / (float)tm.total;

  const float amp = tm.state == T_INTRO ? 10 : tm.state == T_PAUSED ? 2 : 4;
  const float base = -8 + (1 - level) * (SCREEN_H + 16);
  const float s = animSecs(now);
  int surface[COLS];
  for (int i = 0; i < COLS; i++) {
    const int x = i * CELL;
    const float wave = amp * sinf(x * 0.04f + s * 2.6f) + amp * 0.5f * sinf(x * 0.11f - s * 4.1f);
    surface[i] = (int)roundf((base + wave) / CELL) * CELL;
  }
  auto surfaceAt = [&](int x) { return surface[constrain(x / CELL, 0, COLS - 1)]; };

  if (level > 0)
    for (int i = 0; i < COLS; i++) g.fillRect(i * CELL, surface[i], CELL, SCREEN_H - surface[i], pal::body);

  if (tm.state == T_RUNNING) {
    const float t = (now % 3600000UL);
    for (int i = 0; i < 7; i++) {
      const int bx = ((i * 47 + 13) % 76) * CELL + CELL;
      const int by = (int)roundf((SCREEN_H - fmodf(t * (0.018f + i * 0.004f) + i * 61, SCREEN_H + 20)) / CELL) * CELL;
      if (by > surfaceAt(bx) + CELL) g.fillRect(bx, by, CELL, CELL, pal::bg);
    }
  }

  const Act a = timerAct(tm, now);
  const TockSprite& sp = TOCK_POSES[a.pose];
  const int x0 = (SCREEN_W - TOCK_W * PX) / 2 + a.shake * 6;
  const int y0 = 192 - (sp.h + a.lift) * PX;
  for (int r = 0; r < sp.h; r++)
    for (int c = 0; c < TOCK_W; c++) {
      if (sp.rows[r][c] != '#') continue;
      const int cy = y0 + r * PX;
      for (int sx = 0; sx < PX; sx += CELL) {
        const int x = x0 + c * PX + sx;
        const int dry = constrain(surfaceAt(x) - cy, 0, PX);
        g.fillRect(x, cy, CELL, dry, pal::body);
        g.fillRect(x, cy + dry, CELL, PX - dry, pal::bg);
      }
    }
  drawEffects(g, a, x0, y0, PX);

  // the time along the bottom, under Tock's feet, cut at the waterline the same way:
  // mustard where it is dry, the background color where the fluid covers it
  if (tm.state == T_DONE) return;
  char text[12];
  clockText(text, sizeof text, remaining(tm));
  constexpr int TY = 204;
  int tx = (int)roundf((SCREEN_W - g.textWidth(text, CELL)) / 2.0f / CELL) * CELL;
  for (const char* p = text; *p; p++) {
    const Glyph& gl = glyph(*p);
    const int w = strlen(gl.rows[0]);
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < w; c++) {
        if (gl.rows[r][c] != '#') continue;
        const int cx = tx + c * CELL, cy = TY + r * CELL;
        const int dry = constrain(surfaceAt(cx) - cy, 0, CELL);
        g.fillRect(cx, cy, CELL, dry, pal::body);
        g.fillRect(cx, cy + dry, CELL, CELL - dry, pal::bg);
      }
    tx += (w + 1) * CELL;
  }
}

// Arcade: Tock walks right eating pellets; a hot coffee waits at the end of the line.
static const char* const MUG[] = {"#######..", "########.", "#######.#", "#######.#", "########.", "#######..", ".#####..."};
static const char* const STEAM[2][3] = {{".#...#.", "#...#..", ".#...#."}, {"#...#..", ".#...#.", "#...#.."}};
static const char* const HEART[] = {".#.#.", "#####", ".###.", "..#.."};
constexpr int MUG_PX = 3, MUG_W = 9 * MUG_PX, MUG_H = 7 * MUG_PX;

inline void drawMug(Gfx& g, int x, int y, uint32_t now, bool steam = true) {
  g.drawGrid(MUG, 7, x, y, MUG_PX, pal::ink);
  g.fillRect(x + MUG_PX, y + 2 * MUG_PX, 5 * MUG_PX, 2 * MUG_PX, pal::body);
  if (steam) g.drawGrid(STEAM[(now / 350) % 2], 3, x + MUG_PX, y - 4 * MUG_PX, MUG_PX, pal::grey);
}

inline void drawArcade(Gfx& g, const Timer& tm, uint32_t now) {
  constexpr int GROUND = 204, PELLETS = 20, PX = 5, TW = TOCK_W * PX;
  g.fillScreen(pal::bg);
  const bool blinkOn = (now / 400) % 2 == 0;
  const float progress = tm.state == T_DONE ? 1 : tm.elapsed / tm.total;
  char buf[16];

  for (int i = 0; i < 18; i++)
    if (((now / 700) + i) % 5) g.fillRect((i * 71 + 23) % 312 + 4, (i * 37 + 11) % 100 + 58, 2, 2, pal::faint);

  g.drawText("SCORE", 12, 10, 2, pal::red);
  snprintf(buf, sizeof buf, "%06lu", (unsigned long)(((uint32_t)tm.elapsed / 1000) * 10));
  g.drawText(buf, 12, 24, 2, pal::ink);
  g.drawText("TIME", SCREEN_W / 2, 10, 2, pal::red, CENTER);
  clockText(buf, sizeof buf, remaining(tm));
  if (tm.state != T_PAUSED || blinkOn) g.drawText(buf, SCREEN_W / 2, 24, 4, pal::body, CENTER);
  g.drawText("STAGE", SCREEN_W - 12, 10, 2, pal::red, RIGHT);
  snprintf(buf, sizeof buf, "%02d", tm.round);
  g.drawText(buf, SCREEN_W - 12, 24, 2, pal::ink, RIGHT);

  for (int row = 0; row < 2; row++)
    for (int bx = row ? -8 : 0; bx < SCREEN_W; bx += 16) g.fillRect(bx + 1, GROUND + row * 9 + 1, 14, 8, pal::dim);

  const int mugX = SCREEN_W - 12 - MUG_W, mugY = GROUND - MUG_H;
  const int startX = 12, endX = mugX - TW - 4;
  const int tx = (int)roundf(startX + (endX - startX) * progress);

  const int mouthY = GROUND - 12 * PX + 7 * PX;
  const int firstX = startX + TW + 6, lastX = mugX - 12;
  for (int i = 0; i < PELLETS; i++) {
    const int x = (int)roundf(firstX + (lastX - firstX) * (i / (float)(PELLETS - 1)));
    if (x > tx + TW - 10) g.fillRect(x, mouthY, 4, 4, pal::ink);
  }

  drawMug(g, mugX, mugY, now);
  if (tm.state == T_DONE) g.drawGrid(HEART, 4, mugX + 2 * MUG_PX, mugY - 26 - (int)(((now - tm.since) / 160) % 8) * 3, 2, pal::red);

  Act a = timerAct(tm, now);
  if (tm.state == T_RUNNING && !petting(tm, now)) {
    const int beat = (now / 260) % 2;
    a.pose = beat ? POSE_SHUFFLE : POSE_STAND;
    a.lift = beat;
  }
  drawTock(g, a, tx, GROUND, PX);

  if (tm.state == T_READY && blinkOn) {
    g.drawText("PRESS B", SCREEN_W / 2, 92, 3, pal::ink, CENTER);
    g.drawText("TO START", SCREEN_W / 2, 116, 2, pal::grey, CENTER);
  } else if (tm.state == T_INTRO) {
    g.drawText(now - tm.since < 950 ? "READY?" : "GO!", SCREEN_W / 2, 96, 4, pal::ink, CENTER);
  } else if (tm.state == T_PAUSED && blinkOn) {
    g.drawText("PAUSED", SCREEN_W / 2, 96, 4, pal::ink, CENTER);
  } else if (tm.state == T_DONE) {
    const char* next = tm.mode == M_POMODORO ? (tm.round >= tm.rounds ? "LONG BREAK" : "COFFEE BREAK") : "ALL DONE";
    g.drawText("STAGE CLEAR!", SCREEN_W / 2, 78, 3, blinkOn ? pal::body : pal::ink, CENTER);
    g.drawText(next, SCREEN_W / 2, 106, 2, pal::ink, CENTER);
    g.drawText("PRESS B", SCREEN_W / 2, 124, 2, pal::grey, CENTER);
  }
}

// Bar: Tock, a clock you can read across the desk, and a 20-segment bar.
inline void drawBar(Gfx& g, const Timer& tm, uint32_t now) {
  constexpr int SEGMENTS = 20, SEG_W = 12, GAP = 2;
  constexpr uint32_t INTRO = 500;
  g.fillScreen(pal::bg);
  const uint32_t left = remaining(tm);
  const bool blinkOn = (now / 500) % 2 == 0;
  char buf[16];

  drawTock(g, timerAct(tm, now), (SCREEN_W - TOCK_W * 8) / 2, 128, 8);
  clockText(buf, sizeof buf, left);
  if (tm.state != T_PAUSED || blinkOn) g.drawText(buf, SCREEN_W / 2, 144, 6, pal::ink, CENTER);

  const int x0 = (SCREEN_W - (SEGMENTS * (SEG_W + GAP) - GAP)) / 2;
  int lit = (int)ceilf(left / (float)tm.total * SEGMENTS);
  if (tm.state == T_INTRO) lit = (int)ceilf(easeOut((now - tm.since) / (float)INTRO) * SEGMENTS);
  if (tm.state == T_DONE) lit = 0;
  for (int i = 0; i < SEGMENTS; i++) {
    uint16_t color = i < lit ? pal::body : pal::faint;
    if (tm.state == T_RUNNING && i == lit - 1 && !blinkOn) color = pal::dim;
    g.fillRect(x0 + i * (SEG_W + GAP), 196, SEG_W, 12, color);
  }
}

// Blocks: 60 blocks; the next one to go shrinks away.
inline void drawBlocks(Gfx& g, const Timer& tm, uint32_t now) {
  constexpr int COLS = 12, ROWS = 5, BLOCKS = COLS * ROWS, BW = 20, BH = 16, GAP = 4;
  constexpr uint32_t INTRO = 900;
  g.fillScreen(pal::bg);
  const uint32_t left = remaining(tm);
  const bool blinkOn = (now / 500) % 2 == 0;
  char buf[16];

  drawTock(g, timerAct(tm, now), (SCREEN_W - TOCK_W * 7) / 2, 112, 7);
  clockText(buf, sizeof buf, left);
  if (tm.state != T_PAUSED || blinkOn) g.drawText(buf, SCREEN_W - 10, 10, 2, pal::grey, RIGHT);

  const int x0 = (SCREEN_W - (COLS * (BW + GAP) - GAP)) / 2, y0 = 124;
  const float exact = left / (float)tm.total * BLOCKS;
  int alive = (int)ceilf(exact);
  const float shrink = alive - exact;
  if (tm.state == T_INTRO) alive = (int)ceilf(easeOut((now - tm.since) / (float)INTRO) * BLOCKS);
  if (tm.state == T_DONE) alive = 0;
  for (int i = 0; i < BLOCKS; i++) {
    const int x = x0 + (i % COLS) * (BW + GAP), y = y0 + (i / COLS) * (BH + GAP);
    g.fillRect(x, y, BW, BH, pal::faint);
    if (i < BLOCKS - alive) continue;
    const int inset = (i == BLOCKS - alive && tm.state != T_INTRO) ? (int)(shrink * 4) * 2 : 0;
    g.fillRect(x + inset, y + inset, BW - inset * 2, BH - inset * 2, pal::body);
  }
}

static const Style STYLES[] = {
  {"FLUID", 1600, drawFluid, 10},
  {"ARCADE", 1500, drawArcade, 227},  // under the ground: the HUD has the top
  {"BAR", 500, drawBar, 10},
  {"BLOCKS", 900, drawBlocks, 10},
};
constexpr int STYLE_COUNT = 4;

// ---------- the break scene ----------
// Tock on a 14 s loop: sip coffee (eyes closed), set the mug down, stroll off, bounce a ball,
// stroll back. Paused: Tock sits. Break over: Tock jumps until you press B.

namespace brk {
constexpr int GROUND = 200, PX = 5, TW = TOCK_W * PX, TH = 12 * PX;
constexpr int HOME_X = 88, PLAY_X = 16, BALL = 8;
constexpr uint32_t LOOP_MS = 14000;
static const int JUMP[] = {0, 3, 5, 6, 6, 5, 3, 0};

inline float lerp(float a, float b, float k) { return a + (b - a) * constrain(k, 0.0f, 1.0f); }

inline Act walking(uint32_t t) {
  const int beat = (t / 260) % 2;
  return still(beat ? POSE_SHUFFLE : POSE_STAND, beat);
}

inline void drawBackToWork(Gfx& g, const Timer& tm, uint32_t now) {
  const bool blinkOn = (now / 350) % 2 == 0;
  char buf[24];
  g.drawText("BACK TO WORK!", SCREEN_W / 2, 22, 3, blinkOn ? pal::body : pal::ink, CENTER);
  if (tm.longBreak) snprintf(buf, sizeof buf, "NEW CYCLE");
  else snprintf(buf, sizeof buf, "ROUND %d OF %d", tm.round + 1, tm.rounds);
  g.drawText(buf, SCREEN_W / 2, 48, 2, pal::grey, CENTER);
  const int lift = JUMP[(now / 75) % 8];
  Act a = still(lift > 0 ? POSE_CHEER : POSE_STAND, lift);
  a.sparks = lift >= 5;
  drawTock(g, a, (SCREEN_W - TOCK_W * 7) / 2, GROUND, 7);
  g.drawText("PRESS B", SCREEN_W / 2, 214, 2, pal::grey, CENTER);
}

inline void draw(Gfx& g, const Timer& tm, uint32_t now, uint32_t anim) {
  g.fillScreen(pal::bg);
  g.fillRect(0, GROUND, SCREEN_W, 2, pal::faint);
  if (tm.state == T_WAITING) return drawBackToWork(g, tm, now);

  char buf[16];
  const char* label = tm.mode == M_INTERVAL ? "REST" : tm.longBreak ? "LONG BREAK" : "BREAK";
  g.drawText(label, 16, 14, 2, pal::grey);
  if (tm.rounds > 1) {
    snprintf(buf, sizeof buf, "%d/%d", tm.round, tm.rounds);
    g.drawText(buf, 16 + g.textWidth(label, 2) + 10, 14, 2, pal::ink);
  }
  clockText(buf, sizeof buf, remaining(tm));
  if (tm.state != T_PAUSED || (now / 500) % 2 == 0) g.drawText(buf, SCREEN_W - 16, 10, 3, pal::ink, RIGHT);

  const uint32_t t = anim % LOOP_MS;
  float x = HOME_X;
  Act a = still(POSE_STAND);
  const float sideX = HOME_X + TW + 2, sideY = GROUND - MUG_H - 10;
  const float groundX = HOME_X + TW + 4, groundY = GROUND - MUG_H;
  const float mouthX = HOME_X + TW / 2 - MUG_W / 2, mouthY = GROUND - TH + 6 * PX;
  float mugX = sideX, mugY = sideY;
  bool sipping = false, ball = false;
  int ballX = 0, ballY = 0, heart = -1;

  if (t < 4000) {
    if (t < 800) {
    } else if (t < 1200) {
      mugX = lerp(sideX, mouthX, (t - 800) / 400.0f);
      mugY = lerp(sideY, mouthY, (t - 800) / 400.0f);
    } else if (t < 3000) {
      sipping = true;
      a = still(POSE_BLINK);
    } else if (t < 3400) {
      mugX = lerp(mouthX, sideX, (t - 3000) / 400.0f);
      mugY = lerp(mouthY, sideY, (t - 3000) / 400.0f);
    } else {
      heart = t - 3400;
    }
  } else if (t < 4400) {
    mugY = lerp(sideY, groundY, (t - 4000) / 400.0f);
  } else if (t < 7400) {
    mugX = groundX, mugY = groundY;
    x = lerp(HOME_X, PLAY_X, (t - 4400) / 3000.0f);
    a = walking(t);
  } else if (t < 10400) {
    mugX = groundX, mugY = groundY;
    x = PLAY_X;
    const float p = ((t - 7400) % 750) / 750.0f;
    const bool hitting = p < 0.12f || p > 0.88f;
    a = still(hitting ? POSE_CHEER : POSE_STAND, hitting ? 1 : 0);
    ball = true;
    ballX = (int)x + TW / 2 - BALL / 2;
    ballY = GROUND - TH - a.lift * PX - BALL - (int)roundf(70 * 4 * p * (1 - p));
  } else if (t < 13400) {
    mugX = groundX, mugY = groundY;
    x = lerp(PLAY_X, HOME_X, (t - 10400) / 3000.0f);
    a = walking(t);
  } else {
    mugX = groundX;
    mugY = lerp(groundY, sideY, (t - 13400) / 600.0f);
  }

  if (tm.state == T_PAUSED) a = still(POSE_SIT);
  const int tx = (int)roundf(x);
  if (!sipping) drawMug(g, (int)roundf(mugX), (int)roundf(mugY), now);
  drawTock(g, a, tx, GROUND, PX);
  if (sipping) drawMug(g, (int)roundf(mouthX), (int)roundf(mouthY), now);  // in front of the face
  if (ball) g.fillRect(ballX, ballY, BALL, BALL, pal::ink);
  if (heart >= 0) g.drawGrid(HEART, 4, tx + TW / 2 + 16, GROUND - TH - 14 - (heart / 100) * 2, 2, pal::red);
}
}  // namespace brk

// ---------- the app ----------

class TimerApp : public App {
 public:
  TimerApp() {
    cfg.load();
    fresh();
  }

  const char* name() override { return "TIMER"; }

  void subtitle(char* out, size_t n) override {
    char c[12];
    if (tm.state == T_PAUSED) {
      clockText(c, sizeof c, remaining(tm));
      snprintf(out, n, "PAUSED %s", c);
    } else if (tm.state == T_WAITING) {
      snprintf(out, n, "BREAK IS OVER");
    } else {
      clockText(c, sizeof c, cfg.focusMs());
      snprintf(out, n, "%s %s", MODE_OPTS[cfg.mode()], c);
    }
  }

  bool badge() override { return tm.state == T_PAUSED || tm.state == T_WAITING; }

  // A mustard tile with Tock cut out of it in black.
  void drawIcon(Gfx& g, int x, int y, uint32_t now, bool live) override {
    constexpr int PX = 3;
    g.fillRect(x, y, ICON_SIZE, ICON_SIZE, pal::body);
    drawTock(g, live ? idleAct(now) : still(POSE_STAND), x + (ICON_SIZE - TOCK_W * PX) / 2, y + ICON_SIZE - 7, PX, pal::bg);
  }

  Settings settings() override {
    Settings rows = cfg.settings([this](Field f) {
      if (f == F_LOOK) return;
      if (f == F_MODE || tm.state == T_READY) fresh();
    });
    if (!tasks.count()) return rows;
    rows.insert(rows.begin(), Setting{"TASK", tasks.optionNames(), tasks.count() + 1,
                                      [] {
                                        for (int i = 0; i < tasks.count(); i++)
                                          if (tasks.at(i).id == tasks.current()) return i + 1;
                                        return 0;
                                      },
                                      [this](int i) { setTask(i ? tasks.at(i - 1).id : 0); }});
    return rows;
  }

  void enter(uint32_t now) override {
    if (tm.state == T_READY) tm.task = tasks.current();  // the tasks load after the apps are made
  }

  void leave(uint32_t now) override {
    if (tm.state == T_RUNNING || tm.state == T_INTRO) setState(T_PAUSED, now);
    publishStatus();
  }

  void button(Btn b, BtnEv ev, uint32_t now) override {
    if (b == BTN_B && ev == EV_CLICK) {
      switch (tm.state) {
        case T_READY: startFocus(now, 1, true); sys.sound.tone(2200, 60); break;
        case T_DONE:
          if (tm.mode == M_POMODORO) startBreak(now);
          else startFocus(now, 1, true);
          sys.sound.tone(2200, 60);
          break;
        case T_RUNNING: setState(T_PAUSED, now); sys.sound.tone(1400, 60); break;
        case T_PAUSED: setState(T_RUNNING, now); sys.sound.tone(2200, 60); break;
        case T_WAITING: startFocus(now, tm.longBreak ? 1 : tm.round + 1, true); sys.sound.tone(2200, 60); break;
        default: break;
      }
    } else if (b == BTN_A && ev == EV_HOLD) {
      if (tm.state == T_READY) return;
      fresh();
      setState(T_READY, now);
      sys.sound.tone(900, 120);
    } else if (b == BTN_A && ev == EV_CLICK && tasks.count()) {
      setTask(tasks.next(tasks.current()));
      const Task* t = tasks.get(tm.task);
      toast(t ? t->name : "NO TASK", now);
      sys.sound.tone(1600, 30);
    } else if (b == BTN_C && ev == EV_CLICK && between()) {
      const char* label = cfg.nextTime();
      if (tm.state == T_READY) fresh();
      toast(label, now);
      sys.sound.tone(1600, 30);
    } else if (ev == EV_CLICK && b != BTN_B) {
      tm.petAt = now;
      tm.petted = true;
      sys.sound.chirp();
    }
  }

  void update(uint32_t now, float dt) override {
    publishStatus();
    const uint32_t real = lastUpdate ? min<uint32_t>(100, now - lastUpdate) : 0;
    lastUpdate = now;

    if (tm.state == T_INTRO && now - tm.since >= style().introMs) setState(T_RUNNING, now);
    if (tm.state == T_RUNNING) {
      const float before = tm.elapsed;
      tm.elapsed = min<float>(tm.total, tm.elapsed + dt);
      if (tm.phase == PH_FOCUS) sys.focus.addFocus(tm.elapsed - before, now, tm.task);
      else breakAnim += real;
      if (tm.mode == M_INTERVAL) {
        const int left = (int)ceilf((tm.total - tm.elapsed) / 1000);
        if (left <= 3 && left > 0 && left != lastCount) sys.sound.tone(880, 60);
        lastCount = left;
      }
      if (tm.elapsed >= tm.total) phaseEnd(now);
    }

    if (tm.state == T_DONE) {
      const uint32_t el = now - tm.since, cycle = el % ALARM_CYCLE_MS;
      if (el < ALARM_MS && cycle < ALARM_RING_MS) {
        const int slot = (el / ALARM_CYCLE_MS) * 100 + cycle / 150;
        if (slot != lastBeep && slot % 2 == 0) sys.sound.tone(2800, 70);
        lastBeep = slot;
      }
      if (tm.mode == M_POMODORO && el >= ALARM_MS) startBreak(now);
    }

    if (tm.state == T_WAITING && now - jingleAt >= 8000) {
      jingleAt = now;
      sys.sound.jingle();
    }
  }

  void drawLeds(Leds& leds, uint32_t now) override {
    if (tm.state == T_READY || tm.state == T_INTRO) return;
    if (tm.state == T_WAITING) {
      const float flash = (now / 120) % 2 ? 1 : 0.3f;
      for (int i = 0; i < LEDS_PER_BAR; i++) {
        leds.set(i, hue(now * 0.4f + i * 60), flash);
        leds.set(LEDS_PER_BAR + i, hue(now * 0.4f + i * 60 + 180), 1.3f - flash);
      }
      return;
    }
    if (tm.state == T_DONE) {
      const uint32_t el = now - tm.since, cycle = el % ALARM_CYCLE_MS;
      const bool ringing = el < ALARM_MS && cycle < ALARM_RING_MS;
      const float level = ringing ? ((cycle / 150) % 2 ? 0 : 1) : 0.6f;
      for (int i = 0; i < LEDS_PER_BAR; i++) leds.setBoth(i, tasks.rgb(tm.task, 0xe7ae45), level);
      return;
    }
    const uint32_t color = tm.phase == PH_BREAK ? 0x3fb8a8 : tasks.rgb(tm.task, 0xe7ae45);
    const float filled = tm.elapsed / tm.total * LEDS_PER_BAR;
    const float breathe = tm.state == T_PAUSED ? 0.45f + 0.35f * sinf(animSecs(now) * 2) : 1;
    for (int i = 0; i < LEDS_PER_BAR; i++) {
      const float level = constrain(filled - i, 0.0f, 1.0f);
      if (level > 0) leds.setBoth(i, color, level * breathe);
    }
  }

  void draw(Gfx& g, uint32_t now) override {
    if (tm.phase == PH_BREAK) {
      brk::draw(g, tm, now, breakAnim);
    } else {
      style().draw(g, tm, now);
      drawTag(g);
    }

    if (now < toastUntil) {  // what A or C just changed
      const char* text = toastText;
      constexpr int SCALE = 3, PAD = 12;
      const int w = g.textWidth(text, SCALE) + PAD * 2, h = 5 * SCALE + PAD * 2;
      const int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
      g.fillRect(x - 2, y - 2, w + 4, h + 4, pal::body);
      g.fillRect(x, y, w, h, pal::bg);
      g.drawText(text, SCREEN_W / 2, y + PAD, SCALE, pal::ink, CENTER);
    }
  }

 private:
  TimerConfig cfg;
  Timer tm;
  int lastBeep = -1, lastCount = -1;
  uint32_t toastUntil = 0, breakAnim = 0, lastUpdate = 0, jingleAt = 0;
  char toastText[TASK_NAME_MAX + 1] = "";

  void toast(const char* text, uint32_t now) {
    strlcpy(toastText, text, sizeof toastText);
    toastUntil = now + 900;
  }

  // For the Mac's menu bar.
  void publishStatus() {
    static const char* const NAMES[] = {"ready", "running", "running", "paused", "done", "waiting"};
    timerStatus.state = NAMES[tm.state];
    timerStatus.onBreak = tm.phase == PH_BREAK;
    timerStatus.leftMs = remaining(tm);
    timerStatus.totalMs = tm.total;
    timerStatus.round = tm.round;
    timerStatus.rounds = tm.rounds;
    timerStatus.task = tm.task;
  }

  // Between countdowns, when C sets the time.
  bool between() const {
    return tm.state == T_READY || tm.state == T_WAITING || (tm.state == T_DONE && tm.mode != M_POMODORO);
  }

  // Applies straight away: from now on, focus counts for this task.
  void setTask(int id) {
    tasks.setCurrent(id);
    tm.task = tasks.current();
  }

  // Which task this focus counts for: a square in its color and the name, on a backing plate so
  // it reads over the fluid too. "NO TASK" gets a hollow square.
  void drawTag(Gfx& g) {
    if (!tasks.count()) return;
    const Task* t = tasks.get(tm.task);
    const int x = 12, y = style().tagY;
    const char* name = t ? t->name : "NO TASK";
    const int w = 14 + g.textWidth(name, 2);
    g.fillRect(x - 4, y - 4, w + 8, 18, pal::bg);
    g.fillRect(x, y + 1, 8, 8, t ? tasks.color(t->id, pal::body) : pal::grey);
    if (!t) g.fillRect(x + 2, y + 3, 4, 4, pal::bg);
    g.drawText(name, x + 14, y, 2, t ? pal::ink : pal::grey);
  }

  const Style& style() const { return STYLES[cfg.get(F_LOOK)]; }

  void fresh() {
    tm = Timer();
    tm.task = tasks.current();
    tm.mode = cfg.mode();
    tm.rounds = cfg.rounds();
    tm.total = cfg.focusMs();
  }

  void setState(TState s, uint32_t now) {
    tm.state = s;
    tm.since = now;
    lastBeep = -1;
    sys.focus.flush(now);
  }

  void startFocus(uint32_t now, int round, bool intro) {
    tm.phase = PH_FOCUS;
    tm.mode = cfg.mode();
    tm.round = round;
    tm.rounds = cfg.rounds();
    tm.longBreak = false;
    tm.total = cfg.focusMs();
    tm.elapsed = 0;
    tm.task = tasks.current();
    setState(intro ? T_INTRO : T_RUNNING, now);
    lastCount = -1;
  }

  void startBreak(uint32_t now) {
    tm.longBreak = tm.mode == M_POMODORO && tm.round >= tm.rounds;
    tm.phase = PH_BREAK;
    tm.total = cfg.breakMs(tm.longBreak);
    tm.elapsed = 0;
    breakAnim = 0;
    lastCount = -1;
    setState(T_RUNNING, now);
  }

  void phaseEnd(uint32_t now) {
    if (tm.phase == PH_FOCUS) {
      sys.focus.addSession(now);
      if (tm.mode == M_INTERVAL && tm.round < tm.rounds) {
        startBreak(now);
        sys.sound.tone(1760, 200);
      } else {
        setState(T_DONE, now);
      }
    } else if (tm.mode == M_INTERVAL) {
      startFocus(now, tm.round + 1, false);
      sys.sound.tone(1760, 200);
    } else {
      setState(T_WAITING, now);
      jingleAt = now;
      sys.sound.jingle();
    }
  }
};
