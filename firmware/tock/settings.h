// settings.h: the Settings app. Mirrors simulator/src/apps/settings.
//   A  up     B  change (DONE: back home)     C  down
// THEME dark / light, SOUND off or 1-5 (every beep; a sample plays as it changes), WI-FI and
// BLUETOOTH on / off,
// with a live status line for each radio under the list.

#pragma once
#include "home.h"
#include "net.h"
#include "system.h"

static const char* const ON_OFF[] = {"OFF", "ON"};
static const char* const VOLUME_OPTS[] = {"OFF", "1", "2", "3", "4", "5"};

class SettingsApp : public App {
 public:
  explicit SettingsApp(std::function<void(uint32_t)> goHome) : goHome(goHome) {}

  const char* name() override { return "SETTINGS"; }

  void subtitle(char* out, size_t n) override {
    snprintf(out, n, "%s%s", THEME_NAMES[theme()], sys.sound.muted ? " - MUTED" : " THEME");
  }

  // A grey gear on a dark tile; mustard when selected.
  void drawIcon(Gfx& g, int x, int y, uint32_t now, bool live) override {
    static const char* const GEAR[] = {
      ".....###.....", "..#..###..#..", ".###########.", "..#########..", "..###...###..",
      "####.....####", "####.....####", "####.....####", "..###...###..", "..#########..",
      ".###########.", "..#..###..#..", ".....###.....",
    };
    constexpr int PX = 3, SPAN = 13 * PX;
    g.fillRect(x, y, ICON_SIZE, ICON_SIZE, rgb565(0x26231f));
    g.drawGrid(GEAR, 13, x + (ICON_SIZE - SPAN) / 2, y + (ICON_SIZE - SPAN) / 2, PX, live ? rgb565(0xe7ae45) : rgb565(0xa49d90));
  }

  void enter(uint32_t now) override {
    sheet.open("TOCK", [this] { return items(); });
  }

  void button(Btn b, BtnEv ev, uint32_t now) override {
    sheet.button(b, ev);
    if (!sheet.isOpen()) goHome(now);  // DONE
  }

  void draw(Gfx& g, uint32_t now) override {
    sheet.draw(g);
    char buf[48];
    g.fillRect(16, 180, SCREEN_W - 32, 2, pal::faint);
    net::wifiStatus(buf, sizeof buf);
    g.drawText("WI-FI", 16, 192, 2, pal::grey);
    g.drawText(buf, 82, 192, 2, net::wstate == net::W_ONLINE ? pal::body : pal::ink);
    net::btStatus(buf, sizeof buf);
    g.drawText("BT", 16, 212, 2, pal::grey);
    g.drawText(buf, 82, 212, 2, net::btOn ? pal::body : pal::ink);
  }

 private:
  std::function<void(uint32_t)> goHome;
  SettingsSheet sheet;

  static int theme() { return constrain(sys.prefs.get("sys.theme", 0), 0, 1); }

  Settings items() {
    return {
      {"THEME", THEME_NAMES, 2, [] { return theme(); }, [](int i) {
         sys.prefs.set("sys.theme", i);
         applyTheme(i);
       }},
      {"SOUND", VOLUME_OPTS, Sound::VOLUME_LEVELS, [] { return Sound::volumeLevel(); }, [](int i) {
         sys.prefs.set("sys.volume", i);
         sys.sound.setVolume(i);
         sys.sound.chirp();  // how loud that is
       }},
      // the radios as they are running, not only as saved
      {"WI-FI", ON_OFF, 2, [] { return net::wstate != net::W_OFF ? 1 : 0; }, [](int i) { net::setWifi(i == 1, millis()); }},
      {"BLUETOOTH", ON_OFF, 2, [] { return net::btOn ? 1 : 0; }, [](int i) { net::setBluetooth(i == 1); }},
    };
  }
};
