// sprites.h: drawing Tock. Mirrors simulator/src/sprites/tock.ts; the poses come from
// tock_sprites.h, generated from lab/tock.js.

#pragma once
#include "gfx.h"
#include "tock_sprites.h"

static const char* const GLYPH_SPARK[] = {".#.", "###", ".#."};
static const char* const GLYPH_RING_L[] = {"#.", ".#", "#."};
static const char* const GLYPH_RING_R[] = {".#", "#.", ".#"};

// A hop, in cells, sampled every 117 ms.
static const int HOP[] = {0, 2, 3, 3, 2, 0};
constexpr int HOP_N = 6;

// What Tock is doing this frame.
struct Act {
  TockPose pose = POSE_STAND;
  int lift = 0;         // cells off the ground
  int shake = 0;        // -1, 0 or 1: the alarm wobble
  bool rings = false;   // alarm ring marks by the knob
  bool sparks = false;
};

inline Act still(TockPose pose, int lift = 0) {
  Act a;
  a.pose = pose;
  a.lift = lift;
  return a;
}

inline Act idleAct(uint32_t now) {
  const bool blink = now % 3300 < 140;
  const int beat = (now / 520) % 2;
  return still(blink ? POSE_BLINK : beat ? POSE_SHUFFLE : POSE_STAND, beat);
}

inline int poseHeight(TockPose p) { return TOCK_POSES[p].h; }

inline void drawEffects(Gfx& g, const Act& a, int tx, int ty, int px, uint16_t color = pal::body) {
  if (a.rings) {
    g.drawGrid(GLYPH_RING_L, 3, tx + px, ty - px, px, pal::grey);
    g.drawGrid(GLYPH_RING_R, 3, tx + (TOCK_W - 3) * px, ty - px, px, pal::grey);
  }
  if (a.sparks) {
    const int sp = px < 8 ? px : 8;
    g.drawGrid(GLYPH_SPARK, 3, tx - 3 * sp, ty + 2 * px, sp, color);
    g.drawGrid(GLYPH_SPARK, 3, tx + TOCK_W * px, ty + 3 * px, sp, color);
  }
}

// Tock standing on baseY with its left edge at x.
inline void drawTock(Gfx& g, const Act& a, int x, int baseY, int px, uint16_t color = pal::body) {
  const int tx = x + a.shake * (px / 2 > 1 ? px / 2 : 1);
  const int ty = baseY - (poseHeight(a.pose) + a.lift) * px;
  g.drawGrid(TOCK_POSES[a.pose].rows, TOCK_POSES[a.pose].h, tx, ty, px, color);
  drawEffects(g, a, tx, ty, px, color);
}
