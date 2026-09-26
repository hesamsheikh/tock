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
//   w / l    scan for Wi-Fi networks / Bluetooth devices
//   W<ssid>\t<password>  save a Wi-Fi network (what wifi-setup.sh sends), then a newline
//   K<id|color|name;...>  set the task list (what the Mac app sends), then a newline
//   M<line;line;...>      set the screensaver's lines, then a newline
//   v        speaker test (silent)    m   mic test     L   Bluetooth off and on

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

// A firmware from the Mac starts on probation (update.h): loop() confirms it after a while, and
// until then a crash sends the bootloader back to the previous one.
extern "C" bool verifyRollbackLater() { return true; }
constexpr uint32_t CONFIRM_AFTER_MS = 15000;

bool booting = true;
uint32_t bootAt = 0, lastFrame = 0;

// ---------- screens ----------

void switchTo(Screen* s, uint32_t now) {
  sheet.close();
  if (s == active) return;
  if (active) active->leave(now);
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

// ---------- serial debug ----------

void dumpScreenshot() {
  const uint8_t* buf = (const uint8_t*)canvas.getBuffer();
  const size_t n = SCREEN_W * SCREEN_H;
  auto px = [&](size_t i) { return (uint16_t)(buf[i * 2] << 8 | buf[i * 2 + 1]); };
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
    const uint8_t rec[3] = {(uint8_t)run, buf[i * 2], buf[i * 2 + 1]};
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
      case 'l': net::scanBluetooth(Serial); break;
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

  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H)) {
    canvas.setPsram(true);
    canvas.createSprite(SCREEN_W, SCREEN_H);
  }

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
  if (power::update()) {  // soft-off on USB power
    sys.leds.clear();
    sys.leds.show();
    delay(50);
    return;
  }

  const uint32_t now = millis();
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
    update::draw(gfx, now);
  } else {
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
  canvas.pushSprite(0, 0);
  sys.leds.show();
  net::tick(now);
  update::tick(now);
  static bool confirmed = false;
  if (!confirmed && now - bootAt > CONFIRM_AFTER_MS) confirmed = true, update::confirm();

  // about 30 fps
  const uint32_t spent = millis() - now;
  if (spent < 33) delay(33 - spent);
}
