// home.h: the launcher (a phone-style icon grid) and the settings sheet any screen gets on
// hold-C. Mirrors simulator/src/os/launcher.ts and settings-sheet.ts.

#pragma once
#include "net.h"
#include "system.h"

// ---------- rounded tiles ----------
// Corner cut per 2 px row, top-down. The focus ring sits 2 and 4 px outside a tile.

static const int CORNER[] = {6, 4, 2};
static const int RING_OUTER[] = {8, 6, 4, 2};

inline void roundCorners(Gfx& g, int x, int y, int w, int h, uint16_t bg, const int* profile = CORNER, int n = 3) {
  for (int i = 0; i < n; i++) {
    const int cut = profile[i], top = y + i * 2, bottom = y + h - (i + 1) * 2;
    g.fillRect(x, top, cut, 2, bg);
    g.fillRect(x + w - cut, top, cut, 2, bg);
    g.fillRect(x, bottom, cut, 2, bg);
    g.fillRect(x + w - cut, bottom, cut, 2, bg);
  }
}

inline void roundedRect(Gfx& g, int x, int y, int w, int h, uint16_t color, uint16_t bg, const int* profile = CORNER, int n = 3) {
  g.fillRect(x, y, w, h, color);
  roundCorners(g, x, y, w, h, bg, profile, n);
}

// ---------- launcher ----------
//   A  previous icon     B  open     C  next icon

class Launcher : public Screen {
 public:
  Launcher(App** apps, int count, std::function<void(App*, uint32_t)> open) : apps(apps), count(count), open(open) {}

  void enter(uint32_t now) override { sel = constrain(sys.prefs.get("home.sel", 0), 0, count - 1); }

  void button(Btn b, BtnEv ev, uint32_t now) override {
    if (ev != EV_CLICK) return;
    if (b == BTN_B) {
      sys.sound.tone(2000, 40);
      open(apps[sel], now);
      return;
    }
    const int next = sel + (b == BTN_A ? -1 : 1);
    if (next < 0 || next >= count) {
      nudgeAt = now;
      nudging = true;
      sys.sound.tone(300, 40);
      return;
    }
    sel = next;
    sys.prefs.set("home.sel", sel);
    sys.sound.tone(1500, 20);
  }

  void draw(Gfx& g, uint32_t now) override {
    constexpr int COLS = 4, CELL_W = SCREEN_W / COLS, GRID_TOP = 34, ROW_H = 84;
    g.fillScreen(pal::bg);
    drawStatusBar(g);

    const int page = sel / 8, first = page * 8;
    const int nudge = nudging && now - nudgeAt < 180 ? (((now - nudgeAt) / 30) % 2 ? 2 : -2) : 0;
    char buf[32];
    for (int i = first; i < min(count, first + 8); i++) {
      const int slot = i - first;
      const int x = (slot % COLS) * CELL_W + (CELL_W - ICON_SIZE) / 2 + (i == sel ? nudge : 0);
      const int y = GRID_TOP + (slot / COLS) * ROW_H;
      const bool selected = i == sel;
      if (selected) {
        roundedRect(g, x - 4, y - 4, ICON_SIZE + 8, ICON_SIZE + 8, pal::body, pal::bg, RING_OUTER, 4);
        roundedRect(g, x - 2, y - 2, ICON_SIZE + 4, ICON_SIZE + 4, pal::bg, pal::body);
      }
      apps[i]->drawIcon(g, x, y, now, selected);
      roundCorners(g, x, y, ICON_SIZE, ICON_SIZE, pal::bg);
      if (apps[i]->badge()) roundedRect(g, x + ICON_SIZE - 8, y - 6, 14, 14, pal::red, pal::bg);
      g.drawText(apps[i]->name(), x + ICON_SIZE / 2, y + ICON_SIZE + 8, 2, selected ? pal::ink : pal::grey, CENTER);
    }
    apps[sel]->subtitle(buf, sizeof buf);
    g.drawText(buf, SCREEN_W / 2, 208, 2, pal::grey, CENTER);
  }

 private:
  App** apps;
  int count, sel = 0;
  std::function<void(App*, uint32_t)> open;
  uint32_t nudgeAt = 0;
  bool nudging = false;

  void drawStatusBar(Gfx& g) {
    char buf[8];
    if (clockd::known()) {
      const time_t t = time(nullptr);
      struct tm lt;
      localtime_r(&t, &lt);
      snprintf(buf, sizeof buf, "%02d:%02d", lt.tm_hour, lt.tm_min);
    } else {
      snprintf(buf, sizeof buf, "--:--");
    }
    g.drawText(buf, 12, 10, 2, pal::ink);

    // battery: 2 px outline, a nub, a fill in the IP5306's 25% steps; mustard while charging
    const int level = M5.Power.getBatteryLevel();
    const bool charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    constexpr int BW = 22, BH = 12, BY = 9;
    const int bx = SCREEN_W - 12 - BW - 2;
    g.fillRect(bx, BY, BW, BH, pal::grey);
    g.fillRect(bx + 2, BY + 2, BW - 4, BH - 4, pal::bg);
    g.fillRect(bx + BW, BY + 4, 2, 4, pal::grey);
    const int fill = (int)roundf((BW - 8) * constrain(level, 0, 100) / 100.0f);
    g.fillRect(bx + 4, BY + 4, fill, BH - 8, charging ? pal::body : level <= 25 ? pal::red : pal::ink);

    // Bluetooth, then Wi-Fi, to the battery's left; each hidden while switched off
    //   Bluetooth: mustard while the Mac is connected, blinking while pairing, grey while waiting
    //   Wi-Fi:     mustard online, blinking while joining, red when it failed
    static const char* const BT[] = {"..##.", "#.#.#", ".###.", "..#..", ".###.", "#.#.#", "..##."};
    static const char* const WIFI[] = {".#######.", "#.......#", "..#####..", ".#.....#.", "...###...", "....#...."};
    const bool blink = (millis() / 400) % 2;
    int x = bx - 10;
    if (net::btOn) {
      x -= 10;
      const uint16_t c = net::pairing ? (blink ? pal::body : pal::faint) : net::btClients > 0 ? pal::body : pal::grey;
      g.drawGrid(BT, 7, x, BY - 1, 2, c);
      x -= 10;
    }
    if (net::wstate != net::W_OFF) {
      x -= 18;
      const uint16_t c = net::wstate == net::W_ONLINE       ? pal::body
                         : net::wstate == net::W_CONNECTING ? (blink ? pal::ink : pal::faint)
                         : net::wstate == net::W_FAILED     ? pal::red
                                                            : pal::grey;  // no network set up
      g.drawGrid(WIFI, 6, x, BY, 2, c);
    }
  }
};

// ---------- settings sheet ----------
//   A  up     B  change (or close, on DONE)     C  down     hold C  close

class SettingsSheet {
 public:
  void open(const char* t, std::function<Settings()> source) {
    title = t;
    list = source;
    sel = scroll = 0;
  }
  void close() { list = nullptr; }
  bool isOpen() const { return (bool)list; }

  void button(Btn b, BtnEv ev) {
    if (ev != EV_CLICK || !list) return;
    Settings items = list();
    const int rows = items.size() + 1;
    if (b == BTN_A || b == BTN_C) {
      const int next = sel + (b == BTN_A ? -1 : 1);
      if (next < 0 || next >= rows) return sys.sound.tone(300, 40);
      sel = next;
      sys.sound.tone(1500, 20);
    } else if (sel == (int)items.size()) {
      close();
      sys.sound.tone(1200, 40);
    } else {
      Setting& s = items[sel];
      s.choose((s.index() + 1) % s.count);
      sys.sound.tone(1600, 30);
    }
  }

  void draw(Gfx& g) {
    constexpr int TOP = 40, ROW_H = 26, VISIBLE = (SCREEN_H - TOP - 12) / ROW_H;
    Settings items = list();
    const int rows = items.size() + 1;
    sel = min(sel, (int)items.size());
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;

    g.fillScreen(pal::bg);
    g.drawText(title, 16, 14, 2, pal::ink);
    g.drawText("SETTINGS", SCREEN_W - 16, 14, 2, pal::grey, RIGHT);
    char buf[32];
    for (int r = scroll; r < min(rows, scroll + VISIBLE); r++) {
      const int y = TOP + (r - scroll) * ROW_H;
      const bool selected = r == sel;
      if (r == (int)items.size()) {
        g.fillRect(16, y + 2, SCREEN_W - 32, ROW_H - 4, selected ? pal::body : pal::faint);
        g.drawText("DONE", SCREEN_W / 2, y + 8, 2, selected ? pal::bg : pal::grey, CENTER);
        continue;
      }
      const Setting& s = items[r];
      if (selected) {
        g.fillRect(8, y + 2, SCREEN_W - 16, ROW_H - 4, pal::faint);
        g.fillRect(8, y + 2, 4, ROW_H - 4, pal::body);
      }
      g.drawText(s.label, 20, y + 8, 2, selected ? pal::ink : pal::grey);
      const char* value = s.options[s.index()];
      if (selected) snprintf(buf, sizeof buf, "< %s >", value);
      else snprintf(buf, sizeof buf, "%s", value);
      g.drawText(buf, SCREEN_W - 20, y + 8, 2, selected ? pal::body : pal::ink, RIGHT);
    }
    if (scroll > 0) g.fillRect(SCREEN_W - 10, TOP - 4, 4, 2, pal::grey);
    if (scroll + VISIBLE < rows) g.fillRect(SCREEN_W - 10, SCREEN_H - 8, 4, 2, pal::grey);
  }

 private:
  const char* title = "";
  std::function<Settings()> list;
  int sel = 0, scroll = 0;
};
