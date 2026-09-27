// Tock: a desk companion for the M5Stack FIRE. The firmware twin of simulator/ (the React
// simulator is where screens are designed; this file and its headers follow it).
//
//   A / B / C    the three front buttons. Tap, or hold for 0.6 s.
//   hold B       home, from anywhere
//   hold C       the current screen's settings, from anywhere
//   power key    hold 2 s: off. hold 2 s again: on (see power.h)
//
// Apps: Timer (timer.h), Stats (stats.h), Talk (talk.h), Saver (saver.h), Settings (settings.h).
// Home screen and the hold-C settings sheet: home.h.
// Wi-Fi: run firmware/wifi-setup.sh once with the FIRE plugged in (see net.h).
//
// Serial (115200) debug keys:
//   a b c    tap A, B, C          A B C    hold A, B, C
//   ( )      press / release A (Talk's push-to-talk)
//   s        screenshot dump      r        power chip, clock and memory status
//   x        countdown speed 1x / 10x / 60x
//   T<epoch> set the clock (UTC seconds), then a newline
//   d / D    fill / wipe the focus log with demo history
//   w        scan for Wi-Fi networks
//   W<ssid>\t<password>  save a Wi-Fi network (what wifi-setup.sh sends), then a newline
//   K<id|color|name;...>  set the task list (what the Mac app sends), then a newline
//   M<line;line;...>      set the screensaver's lines, then a newline
//   v        speaker test (silent)    m   mic test     L   Bluetooth off and on
//   P        PSRAM test: 1 MB checked every 5 s for a minute

#include <M5Unified.h>
#include "power.h"
#include "gfx.h"
#include "system.h"
#include "sprites.h"
#include "home.h"
#include "timer.h"
#include "stats.h"
#include "saver.h"
#include "net.h"
#include "mic.h"
#include "talk.h"
#include "settings.h"
#if __has_include("secrets.h") && !defined(TOCK_RELEASE)  // a release is for everyone: none of yours in it
#include "secrets.h"
#endif
#ifndef TOCK_TZ
#define TOCK_TZ "UTC0"  // set yours in secrets.h, see secrets.h.example
#endif

Sys sys;
M5Canvas canvas(&M5.Display);
Gfx gfx(canvas);

TimerApp* timerApp;
StatsApp* statsApp;
SaverApp* saverApp;
TalkApp* talkApp;
SettingsApp* settingsApp;
App* apps[5];
Launcher* launcher;
Screen* active = nullptr;
SettingsSheet sheet;

// PSRAM at 40 MHz. The core's prebuilt libraries start it at 80, where this FIRE's PSRAM hands
// back words shifted by a nibble (serial P tests it). flash.sh links with --wrap=psram_enable, so
// the core's call lands here; flash stays at 80 MHz (PSRAM_CACHE_F80M_S40M, a supported pairing).
extern "C" esp_err_t __real_psram_enable(int mode, int vaddrMode);
extern "C" esp_err_t __wrap_psram_enable(int mode, int vaddrMode) { return __real_psram_enable(0, vaddrMode); }

// A firmware from the Mac starts on probation (update.h): loop() confirms it after a while, and
// until then a crash sends the bootloader back to the previous one.
extern "C" bool verifyRollbackLater() { return true; }
constexpr uint32_t CONFIRM_AFTER_MS = 15000;

bool booting = true;
// where a frame's time goes, in microseconds, smoothed (serial r)
float tDraw = 0, tPush = 0, tLeds = 0, tNet = 0, tAll = 0;
inline void smooth(float& avg, uint32_t us) { avg += (us - avg) * 0.1f; }
uint32_t bootAt = 0, lastFrame = 0;

// ---------- screens ----------

// The frame lives in internal RAM, except while Talk or an update runs: they need that RAM
// themselves (TLS, audio, the update's buffer), so it moves to PSRAM, and back after.
bool canvasFast = false;
uint32_t canvasTriedAt = 0, bootBlock = 0;
void placeCanvas(bool fast) {
  if (fast == canvasFast) return;
  // back to internal RAM only if a block that big is free (moving rebuilds the frame), checked
  // at most every 10 s
  if (fast && canvasTriedAt && millis() - canvasTriedAt < 10000) return;
  if (fast && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < SCREEN_W * SCREEN_H + 8192) {
    canvasTriedAt = millis() | 1;
    return;
  }
  canvasFast = gfx.place(fast);
  canvasTriedAt = fast && !canvasFast ? millis() | 1 : 0;
  Serial.printf("canvas: %s, internal heap %u\n", canvasFast ? "internal RAM" : "PSRAM", ESP.getFreeHeap());
}

void switchTo(Screen* s, uint32_t now) {
  sheet.close();
  if (s == active) return;
  if (active) active->leave(now);
  placeCanvas(s != (Screen*)talkApp && !update::showing(now));  // before Talk's enter() asks for its memory
  active = s;
  active->enter(now);
}

void goHome(uint32_t now) { switchTo(launcher, now); }

void toggleSettings(uint32_t now) {
  if (sheet.isOpen()) {
    sheet.close();
    sys.sound.tone(1200, 40);
    return;
  }
  if (active->settings().empty()) {
    sys.sound.tone(300, 40);
    return;
  }
  const char* title = "HOME";
  for (App* a : apps)
    if (a == active) title = a->name();
  Screen* screen = active;
  sheet.open(title, [screen] { return screen->settings(); });
  sys.sound.tone(2000, 40);
}

// ---------- input ----------

struct PendingEvent {
  Btn b;
  BtnEv ev;
};
PendingEvent serialEvents[8];
int serialEventCount = 0;

void handleEvent(Btn b, BtnEv ev, uint32_t now) {
  if (b == BTN_B && ev == EV_HOLD) {
    if (active != launcher) {
      sys.sound.tone(1200, 40);
      goHome(now);
    }
    return;
  }
  if (b == BTN_C && ev == EV_HOLD) return toggleSettings(now);
  if (sheet.isOpen()) sheet.button(b, ev);
  else active->button(b, ev, now);
}

void pollButtons(uint32_t now) {
  m5::Button_Class* btns[3] = {&M5.BtnA, &M5.BtnB, &M5.BtnC};
  for (int i = 0; i < 3; i++) {
    if (btns[i]->wasPressed()) handleEvent((Btn)i, EV_PRESS, now);
    if (btns[i]->wasHold()) handleEvent((Btn)i, EV_HOLD, now);
    else if (btns[i]->wasClicked()) handleEvent((Btn)i, EV_CLICK, now);
    if (btns[i]->wasReleased()) handleEvent((Btn)i, EV_RELEASE, now);
  }
  for (int i = 0; i < serialEventCount; i++) handleEvent(serialEvents[i].b, serialEvents[i].ev, now);
  serialEventCount = 0;
}

// ---------- PSRAM test (serial P) ----------
// 1 MB of PSRAM filled with a pattern, checked every 5 s for a minute while everything else runs.

namespace psramTest {
inline uint32_t* buf = nullptr;
inline uint32_t startedAt = 0, lastCheck = 0, errors = 0;
constexpr size_t WORDS = 256 * 1024;
inline uint32_t pattern(size_t i) { uint32_t x = i * 2654435761u + 0x9e3779b9u; return x ^ (x >> 15); }

inline void start(uint32_t now) {
  if (!buf) buf = (uint32_t*)ps_malloc(WORDS * 4);
  if (!buf) return (void)Serial.println("psram: no memory");
  for (size_t i = 0; i < WORDS; i++) buf[i] = pattern(i);
  startedAt = lastCheck = now;
  errors = 0;
  Serial.println("psram: filled 1 MB, checking every 5 s for 60 s");
}

inline void tick(uint32_t now) {
  if (!startedAt || now - lastCheck < 5000) return;
  lastCheck = now;
  uint32_t bad = 0;
  for (size_t i = 0; i < WORDS; i++) {
    const uint32_t want = pattern(i), got = buf[i];
    if (got != want) {
      if (bad < 4) Serial.printf("psram:   @%u want %08lx got %08lx\n", (unsigned)(i * 4), (unsigned long)want, (unsigned long)got);
      bad++;
      buf[i] = want;  // count each change once
    }
  }
  errors += bad;
  Serial.printf("psram: %lus, %lu words changed since the last check, %lu in all\n", (unsigned long)((now - startedAt) / 1000),
                (unsigned long)bad, (unsigned long)errors);
  if (now - startedAt >= 60000) startedAt = 0, Serial.println("psram: done");
}
}  // namespace psramTest

// ---------- serial debug ----------

void dumpScreenshot() {
  const size_t n = SCREEN_W * SCREEN_H;
  auto px = [&](size_t i) { return gfx.pixel(i); };
  size_t len = 0;
  for (size_t i = 0; i < n;) {
    size_t run = 1;
    while (i + run < n && run < 255 && px(i + run) == px(i)) run++;
    i += run;
    len += 3;
  }
  Serial.printf("\nTOCKSHOT %u\n", (unsigned)len);
  for (size_t i = 0; i < n;) {
    size_t run = 1;
    while (i + run < n && run < 255 && px(i + run) == px(i)) run++;
    const uint16_t v = px(i);
    const uint8_t rec[3] = {(uint8_t)run, (uint8_t)(v >> 8), (uint8_t)v};
    Serial.write(rec, 3);
    i += run;
  }
  Serial.flush();
}

void handleSerial(uint32_t now) {
  static char line[160];
  static char lineCmd = 0;  // 'T', 'W', 'K' or 'M' while reading the rest of that command's line
  static int lineLen = 0;
  while (Serial.available()) {
    const char ch = Serial.read();
    if (lineCmd) {
      if (ch == '\n' || ch == '\r') {
        line[lineLen] = 0;
        if (lineCmd == 'T') {
          clockd::setEpoch((time_t)atoll(line));
          Serial.printf("clock set, today is day %ld\n", (long)clockd::today());
          sys.focus.flush(now);
        } else if (lineCmd == 'K' || lineCmd == 'M') {  // tasks / saver lines, ';' between lines
          for (char* p = line; *p; p++)
            if (*p == ';') *p = '\n';
          if (lineCmd == 'K') tasks.setAll(line);
          else messages.setAll(line);
          Serial.printf("%s saved\n", lineCmd == 'K' ? "tasks" : "saver lines");
        } else if (char* tab = strchr(line, '\t')) {
          *tab = 0;
          net::saveNetwork(line, tab + 1, now);
          Serial.printf("wifi network saved: %s, connecting\n", line);
        } else {
          Serial.println("wifi setup: expected W<ssid><tab><password>");
        }
        lineCmd = 0;
      } else if (lineLen < (int)sizeof line - 1) {
        line[lineLen++] = ch;
      }
      continue;
    }
    auto queue = [&](Btn b, BtnEv ev) {
      if (serialEventCount < 8) serialEvents[serialEventCount++] = {b, ev};
    };
    switch (ch) {
      case 'a': queue(BTN_A, EV_CLICK); break;
      case 'b': queue(BTN_B, EV_CLICK); break;
      case 'c': queue(BTN_C, EV_CLICK); break;
      case 'A': queue(BTN_A, EV_HOLD); break;
      case 'B': queue(BTN_B, EV_HOLD); break;
      case 'C': queue(BTN_C, EV_HOLD); break;
      case '(': queue(BTN_A, EV_PRESS); break;
      case ')': queue(BTN_A, EV_RELEASE); break;
      case 's': dumpScreenshot(); break;
      case 'r':
        power::printRegisters(Serial);
        Serial.printf("wifi: saved network \"%s\", state %d, status %d, rssi %d\n",
                      sys.prefs.raw().getString("wifi.ssid", "").c_str(), (int)net::wstate, (int)WiFi.status(), WiFi.RSSI());
        Serial.printf("bt: on=%d service=%d advertising=%d clients=%d bonds=%d name=%s\n", net::btOn, net::serviceReady,
                      net::serviceReady && NimBLEDevice::getAdvertising()->isAdvertising(), (int)net::btClients,
                      net::stackReady ? NimBLEDevice::getNumBonds() : -1, net::btName);
        Serial.printf("rtc=%d clock=%d epoch=%ld today=%ld heap=%u psram=%u\n", M5.Rtc.isEnabled(), clockd::known(),
                      (long)time(nullptr), (long)clockd::today(), ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.printf("frame %.1f ms: draw %.1f, push %.1f (%d rows), leds %.1f, net %.1f, canvas in %s\n", tAll / 1000,
                      tDraw / 1000, tPush / 1000, gfx.sentRows, tLeds / 1000, tNet / 1000, canvasFast ? "internal RAM" : "PSRAM");
        Serial.printf("internal RAM: largest block %u now, %u at boot\n", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (unsigned)bootBlock);
        break;
      case 'x':
        sys.timeScale = sys.timeScale == 1 ? 10 : sys.timeScale == 10 ? 60 : 1;
        Serial.printf("countdown speed %gx\n", sys.timeScale);
        break;
      case 'T':
      case 'W':
      case 'K':
      case 'M': lineCmd = ch, lineLen = 0; break;
      case 'w': net::scanWifi(Serial); break;
      case 'L':  // Bluetooth off and on again, like the Settings switch
        net::setBluetooth(false);
        delay(300);
        net::setBluetooth(true);
        Serial.println("bt: restarted");
        break;
      case 'v': {  // speaker test: play 2 s of silence through voice.h, report the real rate (no sound)
        static int16_t zeros[480] = {};
        M5.Speaker.end();
        voice::begin();
        const size_t p0 = voice::played;
        for (int i = 0; i < 100; i++) voice::push(zeros, 480);
        const uint32_t t0 = millis();
        while (voice::queued() && millis() - t0 < 4000) delay(1);
        const uint32_t took = millis() - t0;
        Serial.printf("voice: %u samples in %u ms = %u Hz\n", (unsigned)(voice::played - p0), (unsigned)took,
                      took ? (unsigned)((uint64_t)(voice::played - p0) * 1000 / took) : 0);
        voice::end();
        M5.Speaker.begin();
        break;
      }
      case 'm': {  // mic test: capture 1.5 s at 24 kHz, report rate and level (nothing is sent)
        static int16_t* buf = (int16_t*)ps_malloc(48000 * 2);
        M5.Speaker.end();
        const bool ok = mic::begin(24000);
        const uint32_t t0 = millis();
        size_t got = 0;
        while (millis() - t0 < 1500 && got < 48000 - 1024) {
          got += mic::read(buf + got, 1024);
          delay(5);
        }
        const uint32_t took = millis() - t0;
        mic::end();
        M5.Speaker.begin();
        int32_t peak = 0;
        int64_t sum = 0;
        for (size_t i = 2400; i < got; i++) peak = max<int32_t>(peak, abs(buf[i])), sum += abs(buf[i]);  // skip settling
        Serial.printf("mic: begin %s, %u samples in %lu ms (%lu per s), peak %ld, average %ld\n", ok ? "ok" : "FAILED",
                      (unsigned)got, (unsigned long)took, (unsigned long)(got * 1000 / max<uint32_t>(took, 1)), (long)peak,
                      (long)(got > 2400 ? sum / (int64_t)(got - 2400) : 0));
        break;
      }
      case 'P': psramTest::start(now); break;
      case 'd': sys.focus.seedDemo(tasks.mask()); Serial.println("demo history added"); break;
      case 'D': sys.focus.clear(); Serial.println("focus log wiped"); break;
    }
  }
}

// ---------- Bluetooth pairing ----------
// Over whatever is on screen: the six-digit code the Mac asks for.

void drawPairing() {
  constexpr int X = 24, Y = 48, W = SCREEN_W - 48, H = 144;
  gfx.fillRect(X - 2, Y - 2, W + 4, H + 4, pal::body);
  gfx.fillRect(X, Y, W, H, pal::bg);
  gfx.drawText("BLUETOOTH PAIRING", SCREEN_W / 2, Y + 16, 2, pal::grey, CENTER);
  char code[8];
  snprintf(code, sizeof code, "%06lu", (unsigned long)net::passkey);
  gfx.drawText(code, SCREEN_W / 2, Y + 48, 6, pal::body, CENTER);
  gfx.drawText("TYPE THIS ON YOUR MAC", SCREEN_W / 2, Y + 112, 2, pal::ink, CENTER);
}

// ---------- boot splash ----------

void drawSplash(uint32_t el) {
  gfx.fillScreen(pal::bg);
  if (el < 300) return;
  const int hop = HOP[min<uint32_t>(HOP_N - 1, (el - 300) / 117)];
  drawTock(gfx, still(el < 1000 ? POSE_CHEER : POSE_STAND, hop), (SCREEN_W - TOCK_W * 8) / 2, 150, 8);
  gfx.drawText("TOCK", SCREEN_W / 2, 172, 4, pal::body, CENTER);
}

// ---------- main ----------

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  power::begin();  // on battery this waits for a 2 s hold, or switches back off

  setenv("TZ", TOCK_TZ, 1);
  tzset();
  if (M5.Rtc.isEnabled()) M5.Rtc.setSystemTimeFromRtc();

  M5.Display.setBrightness(140);
  for (auto* b : {&M5.BtnA, &M5.BtnB, &M5.BtnC}) b->setHoldThresh(HOLD_MS);

  bootBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  canvasFast = gfx.place(true);  // before Wi-Fi and Bluetooth take their share of internal RAM

  sys.prefs.begin();
  applyTheme(sys.prefs.get("sys.theme", 0));
  sys.sound.setVolume(Sound::volumeLevel());
  sys.leds.begin();
  sys.focus.begin(&sys.prefs.raw());
  tasks.begin();
  messages.begin();
  randomSeed(esp_random());

  timerApp = new TimerApp();
  statsApp = new StatsApp();
  saverApp = new SaverApp();
  talkApp = new TalkApp();
  settingsApp = new SettingsApp([](uint32_t now) { goHome(now); });
  apps[0] = timerApp;
  apps[1] = statsApp;
  apps[2] = talkApp;
  apps[3] = saverApp;
  apps[4] = settingsApp;
  launcher = new Launcher(apps, 5, [](App* a, uint32_t now) { switchTo(a, now); });

#ifdef TOCK_WIFI_SSID
  // an old secrets.h: bring its network over once, then Wi-Fi setup lives on the device
  if (!net::haveNetwork()) net::saveNetwork(TOCK_WIFI_SSID, TOCK_WIFI_PASS, millis());
#endif
  net::begin(TOCK_TZ, millis());
  bootAt = lastFrame = millis();
  Serial.println("tock " TOCK_VERSION " ready");
}

void loop() {
  M5.update();
  static bool wasOff = false;
  if (power::update()) {  // soft-off on USB power
    sys.leds.clear();
    sys.leds.show();
    wasOff = true;
    delay(50);
    return;
  }
  if (wasOff) wasOff = false, gfx.invalidate();  // whole frame again after waking

  const uint32_t now = millis();
  const uint32_t u0 = micros();
  const float dt = min<uint32_t>(100, now - lastFrame);
  lastFrame = now;

  handleSerial(now);
  sys.leds.clear();

  if (booting) {
    const uint32_t el = now - bootAt;
    if (el >= 300 && el - dt < 300) sys.sound.chirp();
    drawSplash(el);
    if (el >= 1500) {
      booting = false;
      switchTo(launcher, now);
    }
  } else if (update::showing(now)) {
    if (active != launcher) goHome(now);  // lets Talk and the rest let go of their memory
    placeCanvas(false);
    update::draw(gfx, now);
  } else {
    if (active != (Screen*)talkApp) placeCanvas(true);  // back from an update, say
    sys.sound.hushed = active->quiet();
    pollButtons(now);
    sys.sound.hushed = active->quiet();
    active->update(now, dt * sys.timeScale);
    if (sheet.isOpen()) sheet.draw(gfx);
    else active->draw(gfx, now);
    active->drawLeds(sys.leds, now);
    if (net::pairing) drawPairing();
  }

  sys.sound.update(now);
  const uint32_t u1 = micros();
  gfx.present(M5.Display);
  const uint32_t u2 = micros();
  sys.leds.show();
  const uint32_t u3 = micros();
  net::tick(now);
  update::tick(now);
  const uint32_t u4 = micros();
  smooth(tDraw, u1 - u0), smooth(tPush, u2 - u1), smooth(tLeds, u3 - u2), smooth(tNet, u4 - u3), smooth(tAll, u4 - u0);
  psramTest::tick(now);
  static bool confirmed = false;
  if (!confirmed && now - bootAt > CONFIRM_AFTER_MS) confirmed = true, update::confirm();

  // up to 60 fps: most frames only send the rows that changed, so they take a few ms
  const uint32_t spent = millis() - now;
  if (spent < 16) delay(16 - spent);
}
