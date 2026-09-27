// gfx.h: drawing on the 320 x 240 frame. Mirrors simulator/src/device/gfx.ts and font.ts:
// everything is integer rectangles, plus a 3 x 5 pixel font and '#'-grid sprites.

#pragma once
#include <M5Unified.h>

constexpr int SCREEN_W = 320, SCREEN_H = 240;

constexpr uint16_t rgb565(uint32_t rgb) {
  return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
}

// The palette (simulator/src/device/palette.ts), in two themes. pal:: holds the current one;
// applyTheme swaps it, and everything drawn after that follows. The screensaver ignores the
// theme and always uses dark::.
//   bg the screen, ink text, body Tock's mustard, grey quieter text, faint empty slots, red alerts;
//   umber, dim, amber: steps from faint to body
struct Palette {
  uint16_t bg, body, ink, grey, faint, umber, dim, amber, red;
};

namespace dark {
constexpr uint16_t bg = 0x0000, body = rgb565(0xe7ae45), ink = rgb565(0xf2ede3), grey = rgb565(0x6e6a62),
                   faint = rgb565(0x1c1a17), umber = rgb565(0x3d2f14), dim = rgb565(0x6b5020),
                   amber = rgb565(0xa67c33), red = rgb565(0xe8574a);
}  // namespace dark

constexpr Palette THEME_DARK = {dark::bg, dark::body, dark::ink, dark::grey, dark::faint, dark::umber, dark::dim, dark::amber, dark::red};
constexpr Palette THEME_LIGHT = {rgb565(0xf3eee3), rgb565(0xe7ae45), rgb565(0x2b2925), rgb565(0xa49d90), rgb565(0xe2dbcd),
                                 rgb565(0xebdcb8), rgb565(0xe3c68c), rgb565(0xe0b366), rgb565(0xd9483b)};

namespace pal {
inline uint16_t bg = dark::bg, body = dark::body, ink = dark::ink, grey = dark::grey, faint = dark::faint,
                umber = dark::umber, dim = dark::dim, amber = dark::amber, red = dark::red;
}  // namespace pal

static const char* const THEME_NAMES[] = {"DARK", "LIGHT"};

inline void applyTheme(int i) {
  const Palette& t = i == 1 ? THEME_LIGHT : THEME_DARK;
  pal::bg = t.bg, pal::body = t.body, pal::ink = t.ink, pal::grey = t.grey, pal::faint = t.faint;
  pal::umber = t.umber, pal::dim = t.dim, pal::amber = t.amber, pal::red = t.red;
}

enum Align { LEFT, CENTER, RIGHT };

struct Glyph {
  char c;
  const char* rows[5];
};

// 3 x 5, with M, N and W wider so they don't read as H.
static const Glyph FONT[] = {
  {'0', {"###", "#.#", "#.#", "#.#", "###"}}, {'1', {".#.", "##.", ".#.", ".#.", "###"}},
  {'2', {"###", "..#", "###", "#..", "###"}}, {'3', {"###", "..#", ".##", "..#", "###"}},
  {'4', {"#.#", "#.#", "###", "..#", "..#"}}, {'5', {"###", "#..", "###", "..#", "###"}},
  {'6', {"###", "#..", "###", "#.#", "###"}}, {'7', {"###", "..#", "..#", ".#.", ".#."}},
  {'8', {"###", "#.#", "###", "#.#", "###"}}, {'9', {"###", "#.#", "###", "..#", "###"}},
  {'A', {".#.", "#.#", "###", "#.#", "#.#"}}, {'B', {"##.", "#.#", "##.", "#.#", "##."}},
  {'C', {".##", "#..", "#..", "#..", ".##"}}, {'D', {"##.", "#.#", "#.#", "#.#", "##."}},
  {'E', {"###", "#..", "##.", "#..", "###"}}, {'F', {"###", "#..", "##.", "#..", "#.."}},
  {'G', {".##", "#..", "#.#", "#.#", ".##"}}, {'H', {"#.#", "#.#", "###", "#.#", "#.#"}},
  {'I', {"###", ".#.", ".#.", ".#.", "###"}}, {'J', {"..#", "..#", "..#", "#.#", ".#."}},
  {'K', {"#.#", "#.#", "##.", "#.#", "#.#"}}, {'L', {"#..", "#..", "#..", "#..", "###"}},
  {'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#"}}, {'N', {"#..#", "##.#", "#.##", "#..#", "#..#"}},
  {'O', {".#.", "#.#", "#.#", "#.#", ".#."}}, {'P', {"##.", "#.#", "##.", "#..", "#.."}},
  {'Q', {".#.", "#.#", "#.#", "##.", ".##"}}, {'R', {"##.", "#.#", "##.", "#.#", "#.#"}},
  {'S', {".##", "#..", ".#.", "..#", "##."}}, {'T', {"###", ".#.", ".#.", ".#.", ".#."}},
  {'U', {"#.#", "#.#", "#.#", "#.#", "###"}}, {'V', {"#.#", "#.#", "#.#", "#.#", ".#."}},
  {'W', {"#...#", "#...#", "#.#.#", "##.##", "#...#"}}, {'X', {"#.#", "#.#", ".#.", "#.#", "#.#"}},
  {'Y', {"#.#", "#.#", ".#.", ".#.", ".#."}}, {'Z', {"###", "..#", ".#.", "#..", "###"}},
  {':', {".", "#", ".", "#", "."}}, {'!', {"#", "#", "#", ".", "#"}}, {'.', {".", ".", ".", ".", "#"}},
  {'?', {"###", "..#", ".#.", "...", ".#."}}, {'-', {"...", "...", "###", "...", "..."}},
  {'/', {"..#", "..#", ".#.", "#..", "#.."}}, {'<', {"..#", ".#.", "#..", ".#.", "..#"}},
  {'>', {"#..", ".#.", "..#", ".#.", "#.."}}, {' ', {"...", "...", "...", "...", "..."}},
  {'%', {"#.#", "..#", ".#.", "#..", "#.#"}},
};

inline const Glyph& glyph(char c) {
  for (const Glyph& g : FONT)
    if (g.c == c) return g;
  return FONT[sizeof(FONT) / sizeof(FONT[0]) - 1];  // space
}

// The frame is 8-bit (each color gets one of 256 palette slots the first time it's drawn), in four
// strips of 60 rows. A strip is 19 KB, small enough to find room in internal RAM, where drawing
// is quick (PSRAM is slow to write), even once Wi-Fi and TLS have cut that RAM into pieces; one
// that doesn't fit goes to PSRAM. present() then sends the display only the rows that changed.
class Gfx {
 public:
  static constexpr int STRIPS = 4, STRIP_H = SCREEN_H / STRIPS;
  static constexpr size_t STRIP_BYTES = SCREEN_W * STRIP_H;
  static constexpr size_t KEEP_FREE = 24 * 1024;  // internal RAM left for Wi-Fi, Bluetooth and the rest

  explicit Gfx(M5GFX& lcd) : lcd(lcd), s{M5Canvas(&lcd), M5Canvas(&lcd), M5Canvas(&lcd), M5Canvas(&lcd)} {}

  // (Re)make the strips: in internal RAM where there's room if `fast`, else in PSRAM. Returns how
  // many are in internal RAM. The whole frame goes to the display next time.
  int place(bool fast) {
    for (auto& c : s) c.deleteSprite();
    inside = 0;
    for (auto& c : s) {
      const bool room = fast && ESP.getFreeHeap() >= STRIP_BYTES + KEEP_FREE &&
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= STRIP_BYTES;
      c.setColorDepth(lgfx::palette_8bit);
      c.setPsram(!room);
      bool ok = c.createSprite(SCREEN_W, STRIP_H);
      if (!ok && room) {
        c.setPsram(true);
        ok = c.createSprite(SCREEN_W, STRIP_H);
      } else if (ok && room) {
        inside++;
      }
      c.createPalette();
      for (int i = 0; i < used; i++) c.setPaletteColor(i, pal565[i]);
    }
    full = true;
    return inside;
  }
  int internalStrips() const { return inside; }

  void fillScreen(uint16_t color) {
    const uint8_t k = idx(color);
    for (auto& c : s) c.fillScreen(k);
  }

  void fillRect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    rect(x, y, w, h, idx(color));
  }

  // Every '#' in the grid becomes a px x px square.
  void drawGrid(const char* const* rows, int n, int x, int y, int px, uint16_t color, bool flip = false) {
    const int w = strlen(rows[0]);
    const uint8_t k = idx(color);
    for (int r = 0; r < n; r++)
      for (int col = 0; col < w; col++)
        if (rows[r][flip ? w - 1 - col : col] == '#') rect(x + col * px, y + r * px, px, px, k);
  }

  int textWidth(const char* s, int scale) {
    int w = 0;
    for (; *s; s++) w += (strlen(glyph(*s).rows[0]) + 1) * scale;
    return w ? w - scale : 0;
  }

  // y is the top of the glyphs; text is 5 * scale tall.
  void drawText(const char* s, int x, int y, int scale, uint16_t color, Align align = LEFT) {
    if (align == CENTER) x -= textWidth(s, scale) / 2;
    else if (align == RIGHT) x -= textWidth(s, scale);
    for (; *s; s++) {
      const Glyph& g = glyph(*s);
      drawGrid(g.rows, 5, x, y, scale, color);
      x += (strlen(g.rows[0]) + 1) * scale;
    }
  }

  // Send the frame to the display: only the rows that changed since the last one (a hash per row),
  // in bands, with short unchanged gaps sent along rather than split.
  void present() {
    constexpr int WORDS = SCREEN_W / 4, GAP = 6;
    int from = -1, last = -1;
    sentRows = 0;
    for (int y = 0; y < SCREEN_H; y++) {
      const uint32_t* px = (const uint32_t*)s[y / STRIP_H].getBuffer() + (y % STRIP_H) * WORDS;
      uint32_t h = 2166136261u;
      for (int i = 0; i < WORDS; i++) h = (h ^ px[i]) * 16777619u;
      if (h == rowHash[y] && !full) continue;
      rowHash[y] = h;
      if (from >= 0 && y - last > GAP) band(from, last);
      if (from < 0 || y - last > GAP) from = y;
      last = y;
    }
    if (from >= 0) band(from, last);
    full = false;
  }

  void invalidate() { full = true; }  // the display lost what it showed: send all of it next time
  int sentRows = 0;                    // in the last present() (serial r)

  // The frame as RGB565, pixel by pixel (screenshots).
  uint16_t pixel(int i) const {
    const int y = i / SCREEN_W;
    return pal565[((const uint8_t*)s[y / STRIP_H].getBuffer())[(y % STRIP_H) * SCREEN_W + i % SCREEN_W]];
  }

 private:
  M5GFX& lcd;
  M5Canvas s[STRIPS];
  int inside = 0;
  uint16_t pal565[256];
  int used = 0;
  uint32_t keys[512] = {};  // color + 1 (0 = empty), open addressing
  uint8_t vals[512];
  uint32_t rowHash[SCREEN_H] = {};
  bool full = true;

  // A rectangle, into the strips it touches (each one clips to itself).
  void rect(int x, int y, int w, int h, uint8_t k) {
    const int a = max(0, y / STRIP_H), b = min(STRIPS - 1, (y + h - 1) / STRIP_H);
    for (int i = a; i <= b; i++) s[i].fillRect(x, y - i * STRIP_H, w, h, k);
  }

  uint8_t idx(uint16_t color) {
    uint32_t h = (color * 2654435761u) >> 23;  // 9 bits
    while (keys[h]) {
      if (keys[h] == color + 1u) return vals[h];
      h = (h + 1) & 511;
    }
    uint8_t v;
    if (used < 256) {
      v = used++;
      pal565[v] = color;
      for (auto& c : s) c.setPaletteColor(v, color);
    } else {
      v = nearest(color);  // all 256 taken (Tock uses a few dozen): the closest one
    }
    keys[h] = color + 1u;
    vals[h] = v;
    return v;
  }

  uint8_t nearest(uint16_t color) const {
    auto ch = [](uint16_t c, int i) { return i == 0 ? (c >> 11) << 1 : i == 1 ? (c >> 5) & 63 : (c & 31) << 1; };
    int best = 0, bestD = INT32_MAX;
    for (int i = 0; i < used; i++) {
      int d = 0;
      for (int k = 0; k < 3; k++) d += (ch(color, k) - ch(pal565[i], k)) * (ch(color, k) - ch(pal565[i], k));
      if (d < bestD) bestD = d, best = i;
    }
    return best;
  }

  // Rows y0..y1 to the display, from each strip they cross.
  void band(int y0, int y1) {
    sentRows += y1 - y0 + 1;
    lcd.setClipRect(0, y0, SCREEN_W, y1 - y0 + 1);
    for (int i = y0 / STRIP_H; i <= y1 / STRIP_H; i++) s[i].pushSprite(&lcd, 0, i * STRIP_H);
    lcd.clearClipRect();
  }
};

// "M:SS", rounding up so the clock reads 0:00 only at the very end.
inline void clockText(char* out, size_t n, uint32_t ms) {
  const uint32_t s = (ms + 999) / 1000;
  snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// Seconds since boot, wrapped hourly, so wave math stays precise in float.
inline float animSecs(uint32_t now) { return (now % 3600000UL) * 0.001f; }

inline float easeOut(float k) {
  k = k < 0 ? 0 : k > 1 ? 1 : k;
  return 1 - (1 - k) * (1 - k) * (1 - k);
}
