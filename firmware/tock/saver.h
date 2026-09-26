// saver.h: the screensaver. Mirrors simulator/src/apps/saver.
//   A  previous line     B  next background (PLASMA, WARP, RAIN)     C  next line
//   hold C: settings (background, how long each line stays: 1 minute or more)
// A quiet screen: no sound at all. WARP and RAIN stay dim; PLASMA fills the screen with Tock's colors.
// The lines are the user's own (tasks.h Messages, made in the Mac app); each is as large as it fits.

#pragma once
#include "system.h"
#include "tasks.h"

namespace saverfx {

inline float rnd(int i, int k) {
  const float x = sinf(i * 127.1f + k * 311.7f) * 43758.5453f;
  return x - floorf(x);
}

// PLASMA: slow waves of Tock's own colors in chunky 8 px blocks: a ring through the palette,
// dark to mustard and back, so the cycle wraps without a seam.
static const uint32_t PLASMA_RGB[] = {0x1c1a17, 0x3d2f14, 0x6b5020, 0xa67c33, 0xe7ae45, 0xa67c33, 0x6b5020, 0x3d2f14};
constexpr int PLASMA_N = 8;

inline uint32_t plasmaRgb(int x, int y, uint32_t now) {
  const float t = animSecs(now);
  const float v = sinf(x * 0.024f + t * 0.3f) + sinf(y * 0.032f - t * 0.23f) + sinf((x + y) * 0.016f + t * 0.17f);
  const int i = (int)floorf((v + 3) / 6 * PLASMA_N + t * 0.15f) % PLASMA_N;  // the palette also drifts, very slowly
  return PLASMA_RGB[(i + PLASMA_N) % PLASMA_N];
}

inline void plasma(Gfx& g, uint32_t now) {
  for (int y = 0; y < SCREEN_H; y += 8)
    for (int x = 0; x < SCREEN_W; x += 8) g.fillRect(x, y, 8, 8, rgb565(plasmaRgb(x, y, now)));
}

// WARP: dim stars streaking out of the middle.
inline void warp(Gfx& g, uint32_t now) {
  const uint16_t trail = rgb565(0x2a2825);
  g.fillScreen(dark::bg);
  const float t = (now % 3600000UL) * 0.00022f;
  for (int i = 0; i < 45; i++) {
    const float sx = rnd(i, 1) * 2 - 1, sy = rnd(i, 2) * 2 - 1;
    const float z = 1 - fmodf(t + rnd(i, 3), 1);
    auto px = [&](float zz) { return SCREEN_W / 2 + sx * 70 / (zz + 0.04f); };
    auto py = [&](float zz) { return SCREEN_H / 2 + sy * 55 / (zz + 0.04f); };
    const float x = px(z), y = py(z);
    if (x < -8 || x > SCREEN_W + 8 || y < -8 || y > SCREEN_H + 8) continue;
    const bool near = z < 0.3f;
    const uint16_t color = near ? dark::grey : z < 0.6f ? (i % 4 ? rgb565(0x4a463f) : dark::dim) : trail;
    const float zt = min(1.0f, z + 0.05f), tx = px(zt), ty = py(zt);
    for (int s = 0; s < 3; s++) {
      const float k = s / 3.0f;
      g.fillRect((int)roundf((tx + (x - tx) * k) / 2) * 2, (int)roundf((ty + (y - ty) * k) / 2) * 2, 2, 2, s == 2 ? color : trail);
    }
    g.fillRect((int)roundf(x / 2) * 2, (int)roundf(y / 2) * 2, near ? 4 : 2, near ? 4 : 2, color);
  }
}

// RAIN: slow columns of dim mustard blocks.
inline void rain(Gfx& g, uint32_t now) {
  constexpr int CELL = 8, COLS = SCREEN_W / CELL;
  static const uint16_t TRAIL[] = {dark::dim, rgb565(0x4a3818), dark::umber, rgb565(0x2a2012), rgb565(0x1f180e)};
  constexpr int N = 5;
  g.fillScreen(dark::bg);
  const float t = now % 3600000UL;
  for (int c = 0; c < COLS; c += 2) {
    const float speed = 0.02f + rnd(c, 1) * 0.04f;
    const float span = SCREEN_H + N * CELL * 2;
    const int head = (int)(fmodf(t * speed + rnd(c, 2) * span, span) / CELL);
    for (int k = 0; k < N; k++) {
      const int row = head - k;
      if (row < 0 || row * CELL >= SCREEN_H) continue;
      g.fillRect(c * CELL + 1, row * CELL + 1, CELL - 2, CELL - 2, TRAIL[k]);
    }
  }
}

}  // namespace saverfx

static const char* const EFFECT_NAMES[3] = {"PLASMA", "WARP", "RAIN"};
static const char* const HOLD_OPTS[4] = {"1 MIN", "2 MIN", "5 MIN", "10 MIN"};
static const int HOLD_MIN[4] = {1, 2, 5, 10};

class SaverApp : public App {
 public:
  SaverApp() {
    effect = constrain(sys.prefs.get("saver.effect", 0), 0, 2);
    hold = constrain(sys.prefs.get("saver.lines", 0), 0, 3);
  }

  const char* name() override { return "SAVER"; }
  bool quiet() override { return true; }

  void subtitle(char* out, size_t n) override { snprintf(out, n, "%s BACKGROUND", EFFECT_NAMES[effect]); }

  // A little plasma, drifting when selected.
  void drawIcon(Gfx& g, int x, int y, uint32_t now, bool live) override {
    const uint32_t t = live ? now * 4 : 0;
    for (int by = 0; by < ICON_SIZE; by += 8)
      for (int bx = 0; bx < ICON_SIZE; bx += 8) g.fillRect(x + bx, y + by, 8, 8, rgb565(saverfx::plasmaRgb(bx * 3, by * 3, t)));
  }

  Settings settings() override {
    return {
      {"BACKGROUND", EFFECT_NAMES, 3, [this] { return effect; }, [this](int i) { effect = i; sys.prefs.set("saver.effect", i); }},
      {"EACH LINE", HOLD_OPTS, 4, [this] { return hold; }, [this](int i) { hold = i; sys.prefs.set("saver.lines", i); }},
    };
  }

  void enter(uint32_t now) override { show(0, now); }

  void button(Btn b, BtnEv ev, uint32_t now) override {
    if (ev != EV_CLICK) return;
    if (b == BTN_A) show(msg - 1, now);
    else if (b == BTN_C) show(msg + 1, now);
    else {
      effect = (effect + 1) % 3;
      sys.prefs.set("saver.effect", effect);
    }
  }

  void update(uint32_t now, float dt) override {
    if (now - msgAt >= HOLD_MIN[hold] * 60000UL) show(msg + 1, now);
  }

  void draw(Gfx& g, uint32_t now) override {
    if (effect == 0) saverfx::plasma(g, now);
    else if (effect == 1) saverfx::warp(g, now);
    else saverfx::rain(g, now);
    if (laidOutFor != messages.version) layout(g);

    const Layout& L = layouts[msg];
    const int scale = L.scale;
    const uint32_t el = now - msgAt;
    int shown = el / TYPE_MS;
    const int lineH = 5 * scale, gap = 2 * scale;
    const int top = (SCREEN_H - (L.count * lineH + (L.count - 1) * gap)) / 2;
    const uint16_t color = effect == 0 || msg % 2 == 0 ? dark::ink : dark::body;  // white on the colorful one
    const int shadow = max(2, (int)roundf(scale / 3.0f));
    char text[40];
    for (int i = 0; i < L.count; i++) {
      if (shown <= 0) break;
      const int len = strlen(L.lines[i]);
      const int n = min(shown, len);
      memcpy(text, L.lines[i], n);
      text[n] = 0;
      shown -= len + 1;
      const bool glitching = n < len || el < 220;
      const int jitter = glitching ? (int)roundf(floorf(sinf(animSecs(now) * 50 + i * 7) * 3) * scale / 2.0f) : 0;
      const int x = (SCREEN_W - g.textWidth(L.lines[i], scale)) / 2 + jitter;
      const int y = top + i * (lineH + gap);
      g.drawText(text, x + shadow, y + shadow, scale, dark::bg);
      g.drawText(text, x, y, scale, color);
    }
  }

  // A slow wave up the bars: mustard, or the plasma's colors at the screen's edges.
  void drawLeds(Leds& leds, uint32_t now) override {
    for (int i = 0; i < LEDS_PER_BAR; i++) {
      if (effect == 0) {
        const int y = SCREEN_H - (i + 1) * (SCREEN_H / LEDS_PER_BAR);
        leds.set(i, saverfx::plasmaRgb(0, y, now), 0.7f);
        leds.set(LEDS_PER_BAR + i, saverfx::plasmaRgb(SCREEN_W - 8, y, now), 0.7f);
      } else {
        leds.setBoth(i, 0xe7ae45, 0.15f + 0.45f * (sinf(animSecs(now) / 0.7f - i * 0.9f) + 1) / 2);
      }
    }
  }

 private:
  static constexpr uint32_t TYPE_MS = 45;
  static constexpr int MARGIN = 12;

  struct Layout {
    char lines[4][40];
    int count = 0, scale = 2;
  };
  Layout layouts[MAX_MESSAGES];
  uint32_t laidOutFor = UINT32_MAX;  // the messages version the layouts were made for
  int effect = 0, hold = 1, msg = 0;
  uint32_t msgAt = 0;

  void show(int i, uint32_t now) {
    msg = (i + messages.count()) % messages.count();
    msgAt = now;
  }

  // Word-wrap at this scale; false if some word alone is too wide.
  bool wrap(Gfx& g, const char* text, int s, Layout& out) {
    out.count = 0;
    char line[40] = "", word[40];
    const char* p = text;
    while (*p) {
      int n = 0;
      while (*p && *p != ' ') word[n++] = *p++;
      word[n] = 0;
      while (*p == ' ') p++;
      char next[80];
      snprintf(next, sizeof next, line[0] ? "%s %s" : "%s%s", line, word);
      if (g.textWidth(next, s) <= SCREEN_W - 2 * MARGIN) {
        strncpy(line, next, sizeof line - 1);
      } else {
        if (!line[0] || g.textWidth(word, s) > SCREEN_W - 2 * MARGIN || out.count >= 3) return false;
        strcpy(out.lines[out.count++], line);
        strcpy(line, word);
      }
    }
    strcpy(out.lines[out.count++], line);
    return true;
  }

  // Each message at the largest scale at which it fits the screen.
  void layout(Gfx& g) {
    for (int m = 0; m < messages.count(); m++) {
      Layout& L = layouts[m];
      L.scale = 0;
      for (int s = 14; s >= 2 && !L.scale; s--)
        if (wrap(g, messages.at(m), s, L) && L.count * 5 * s + (L.count - 1) * 2 * s <= SCREEN_H - 2 * MARGIN) L.scale = s;
      if (!L.scale) {  // can't happen with 28 letters, but never draw garbage
        L.scale = 2;
        L.count = 1;
        strlcpy(L.lines[0], messages.at(m), sizeof L.lines[0]);
      }
    }
    msg %= messages.count();
    laidOutFor = messages.version;
  }
};
