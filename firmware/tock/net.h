// net.h: Wi-Fi and Bluetooth, each switched on or off in the Settings app.
//
// Wi-Fi joins the network saved on the device (from the Mac companion app, or once over USB with
// firmware/wifi-setup.sh), sets the clock from the internet, and stays connected while on.
//
// Bluetooth makes Tock visible as "Tock-XXXX" for the Mac companion app (companion/). Every
// characteristic needs an encrypted, authenticated link, so the first connection pairs: the FIRE
// shows a six-digit code and the Mac asks for it. Tock's service:
//   STATUS     read   JSON: name, battery, clock, Wi-Fi, whether an API key is set (never the key),
//                     the day the daily goal was last set, and what the Timer is doing
//   STATS      read   binary focus log for the heatmap, see statsBlob()
//   COMMAND    write  "time:<epoch>" "tz:<POSIX TZ>" "key:<openai key>" "model:<name>" "wifi:<ssid>\t<password>"
//                     "tasks:<lines id|color|name>" (the whole list; id 0 = new)
//                     "msgs:<lines>" (the screensaver's lines; empty = the defaults)
//                     "goal:<hours>" "goalmin:<minutes>" (today's goal, 15 minutes to 16 hours)
//                     "update:<size> <sha256> [<deflated size>]" "update:end" "update:cancel"
//                     (new firmware, update.h)
//   TASKS      read   text: "cur:<current id>", then a line "id|color|name" per task
//   MESSAGES   read   text: the screensaver's lines
//   TASKSTATS  read   binary per-task minutes for the last 14 days, see taskStatsBlob()
//   UPDATE     write  firmware chunks (u32 offset + bytes, without response)
//              read   how the update is going, see update::report()
//
// Serial debug: w = Wi-Fi scan, W<ssid>\t<password>\n = save a network.

#pragma once
#include <NimBLEDevice.h>  // NimBLE, not the core's Bluedroid stack: Bluedroid and Wi-Fi don't fit in IRAM together
#include <WiFi.h>
#include "system.h"
#include "tasks.h"
#include "update.h"

#define TOCK_BLE_SERVICE "7a0c0001-4c3f-4d7e-9b6a-70c6f1a0c0de"
#define TOCK_BLE_STATUS "7a0c0002-4c3f-4d7e-9b6a-70c6f1a0c0de"
#define TOCK_BLE_STATS "7a0c0003-4c3f-4d7e-9b6a-70c6f1a0c0de"
#define TOCK_BLE_COMMAND "7a0c0004-4c3f-4d7e-9b6a-70c6f1a0c0de"
#define TOCK_BLE_TASKS "7a0c0005-4c3f-4d7e-9b6a-70c6f1a0c0de"
#define TOCK_BLE_MESSAGES "7a0c0006-4c3f-4d7e-9b6a-70c6f1a0c0de"
#define TOCK_BLE_TASKSTATS "7a0c0007-4c3f-4d7e-9b6a-70c6f1a0c0de"
#define TOCK_BLE_UPDATE "7a0c0008-4c3f-4d7e-9b6a-70c6f1a0c0de"

constexpr const char* DEFAULT_AI_MODEL = "gpt-realtime-2.1";
constexpr const char* LIVE_BACKEND = "gpt-5.6-terra";  // the Responses model GPT-Live hands the thinking to

namespace net {

// ---------- Wi-Fi ----------

enum WState { W_OFF, W_NO_SETUP, W_CONNECTING, W_ONLINE, W_FAILED };

inline const char* tz = "UTC0";  // replaced in begin(): the Mac's time zone, or TOCK_TZ until it sends one
inline String tzSaved;
inline WState wstate = W_OFF;
inline uint32_t wifiSince = 0;

constexpr uint32_t CONNECT_TIMEOUT_MS = 20000, RETRY_MS = 15000;

inline bool haveNetwork() { return sys.prefs.raw().getString("wifi.ssid", "").length() > 0; }
inline bool wifiEnabled() { return sys.prefs.get("sys.wifi", 1) == 1; }
inline bool online() { return wstate == W_ONLINE; }

inline void wifiConnect(uint32_t now) {
  if (!haveNetwork()) {
    wstate = W_NO_SETUP;
    return;
  }
  const String ssid = sys.prefs.raw().getString("wifi.ssid", "");
  const String pass = sys.prefs.raw().getString("wifi.pass", "");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  configTzTime(tz, "pool.ntp.org", "time.google.com");
  wstate = W_CONNECTING;
  wifiSince = now;
}

inline void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wstate = W_OFF;
}

inline void setWifi(bool on, uint32_t now) {
  sys.prefs.set("sys.wifi", on ? 1 : 0);
  if (on) wifiConnect(now);
  else wifiOff();
}

// Save a network and join it.
inline void saveNetwork(const char* ssid, const char* pass, uint32_t now) {
  sys.prefs.raw().putString("wifi.ssid", ssid);
  sys.prefs.raw().putString("wifi.pass", pass);
  sys.prefs.set("sys.wifi", 1);
  WiFi.disconnect(true);
  wifiConnect(now);
}

// The time zone, as a POSIX TZ string ("STD-1DST-2,M3.5.0,M10.5.0/3"): the Mac sends its own on
// every connection, and it's kept for the days without the Mac.
inline void setTimezone(const char* posix, bool save = true) {
  if (!*posix || (tz == tzSaved.c_str() && tzSaved == posix)) return;
  tzSaved = posix;
  tz = tzSaved.c_str();
  if (save) sys.prefs.raw().putString("sys.tz", posix);
  setenv("TZ", tz, 1);
  tzset();
}

// ---------- the AI key and model, set from the Mac ----------

inline String aiKey() { return sys.prefs.raw().getString("ai.key", ""); }
inline String aiModel() { return sys.prefs.raw().getString("ai.model", DEFAULT_AI_MODEL); }

// ---------- Bluetooth ----------

inline bool btOn = false, stackReady = false, serviceReady = false;
inline char btName[16] = "TOCK";
inline volatile int btClients = 0;
inline volatile bool pairing = false;       // the Mac asked to pair; the code is on screen
inline volatile uint32_t passkey = 0, pairAt = 0;
inline char command[520];
inline volatile bool commandPending = false;  // written by the Bluetooth task, handled in loop()
inline volatile uint32_t securedAt = 0;       // when a Mac's link was last encrypted (Bluetooth task)
// Bumped whenever the characteristics change: a paired Mac caches them, so after an update Tock
// tells it once to look again (a Service Changed indication).
constexpr int GATT_LAYOUT = 3;
inline NimBLECharacteristic *chStatus = nullptr, *chStats = nullptr, *chTasks = nullptr, *chMessages = nullptr,
                           *chTaskStats = nullptr;

inline bool btEnabled() { return sys.prefs.get("sys.bt", 0) == 1; }

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    btClients = btClients + 1;
    Serial.printf("bt: connected %s\n", info.getAddress().toString().c_str());
    // ask the Mac to pair (or to re-encrypt with the saved bond) straight away
    if (!info.isEncrypted()) NimBLEDevice::startSecurity(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    btClients = btClients > 0 ? btClients - 1 : 0;
    pairing = false;
    Serial.printf("bt: disconnected, reason %d\n", reason);
    if (btOn) NimBLEDevice::startAdvertising();  // NimBLE stops advertising while connected: be findable again
  }
  uint32_t onPassKeyDisplay() override {
    passkey = 100000 + esp_random() % 900000;
    pairAt = millis();
    pairing = true;
    return passkey;
  }
  void onConnParamsUpdate(NimBLEConnInfo& info) override {
    Serial.printf("bt: connection interval %.2f ms, latency %u, mtu %u\n", info.getConnInterval() * 1.25f,
                  info.getConnLatency(), info.getMTU());
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    pairing = false;
    securedAt = millis();
    Serial.printf("bt: security done, encrypted=%d authenticated=%d bonded=%d\n", info.isEncrypted(), info.isAuthenticated(), info.isBonded());
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    if (commandPending) return;  // one at a time; the app waits for each write to finish
    const NimBLEAttValue v = c->getValue();
    const size_t n = min<size_t>(v.length(), sizeof(command) - 1);
    memcpy(command, v.data(), n);
    command[n] = 0;
    commandPending = true;
  }
};

// Firmware chunks go straight into the image in PSRAM; a read reports how far it got, fresh.
class UpdateCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const NimBLEAttValue v = c->getValue();
    update::chunk(v.data(), v.length());
  }
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    uint8_t r[14];
    c->setValue(r, update::report(r));
  }
};

inline ServerCallbacks serverCallbacks;
inline CommandCallbacks commandCallbacks;
inline UpdateCallbacks updateCallbacks;

// The stack starts on first use (it takes RAM) and then stays up; "off" stops advertising.
inline void ensureStack() {
  if (stackReady) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(btName, sizeof btName, "Tock-%02X%02X", mac[4], mac[5]);
  NimBLEDevice::init(btName);
  NimBLEDevice::setMTU(517);
  // bonded, man-in-the-middle protected, LE Secure Connections; Tock shows the code, the Mac types it
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  stackReady = true;
}

inline void setBluetooth(bool on) {
  sys.prefs.set("sys.bt", on ? 1 : 0);
  btOn = on;
  if (on) {
    ensureStack();
    if (!serviceReady) {
      NimBLEServer* server = NimBLEDevice::createServer();
      server->setCallbacks(&serverCallbacks, false);
      NimBLEService* svc = server->createService(TOCK_BLE_SERVICE);
      const uint32_t secureRead = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN;
      chStatus = svc->createCharacteristic(TOCK_BLE_STATUS, secureRead, 512);
      chStats = svc->createCharacteristic(TOCK_BLE_STATS, secureRead, 512);
      svc->createCharacteristic(TOCK_BLE_COMMAND, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, 512)
        ->setCallbacks(&commandCallbacks);
      // newer ones go last, so the older ones keep their handles for a Mac that cached them
      chTasks = svc->createCharacteristic(TOCK_BLE_TASKS, secureRead, 512);
      chMessages = svc->createCharacteristic(TOCK_BLE_MESSAGES, secureRead, 512);
      chTaskStats = svc->createCharacteristic(TOCK_BLE_TASKSTATS, secureRead, 512);
      svc->createCharacteristic(TOCK_BLE_UPDATE, secureRead | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE |
                                                   NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, 512)
        ->setCallbacks(&updateCallbacks);
      svc->start();
      NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
      adv->setName(btName);
      adv->addServiceUUID(TOCK_BLE_SERVICE);
      adv->enableScanResponse(true);
      serviceReady = true;
    }
    NimBLEDevice::startAdvertising();
  } else if (serviceReady) {
    NimBLEDevice::stopAdvertising();
    NimBLEServer* server = NimBLEDevice::getServer();
    for (uint16_t id : server->getPeerDevices()) server->disconnect(id);
  }
}

// ---------- what the Mac reads ----------

inline void statusJson(char* out, size_t n) {
  String ssid = WiFi.SSID(), key = aiKey(), model = aiModel();
  ssid.replace("\"", "");
  model.replace("\"", "");
  const char* w = wstate == W_ONLINE ? "online" : wstate == W_CONNECTING ? "connecting" : wstate == W_FAILED ? "failed"
                : wstate == W_NO_SETUP ? "not set up" : "off";
  snprintf(out, n,
           "{\"version\":\"%s\",\"name\":\"%s\",\"battery\":%d,\"charging\":%s,\"clock\":%s,\"wifi\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
           "\"key\":\"%s\",\"model\":\"%s\",\"goalDay\":%d,\"goalMin\":%d,\"timer\":{\"state\":\"%s\",\"break\":%s,"
           "\"left\":%u,\"total\":%u,\"round\":%d,\"rounds\":%d,\"task\":%d}}",
           TOCK_VERSION, btName, (int)M5.Power.getBatteryLevel(), M5.Power.isCharging() == m5::Power_Class::is_charging ? "true" : "false",
           clockd::known() ? "true" : "false", w, wstate == W_ONLINE ? ssid.c_str() : "",
           wstate == W_ONLINE ? WiFi.localIP().toString().c_str() : "",
           key.length() >= 4 ? key.substring(key.length() - 4).c_str() : "", model.c_str(), sys.prefs.get("stats.goalday", -1), goalMinutes(),
           timerStatus.state, timerStatus.onBreak ? "true" : "false", (unsigned)(timerStatus.leftMs / 1000),
           (unsigned)(timerStatus.totalMs / 1000), timerStatus.round, timerStatus.rounds, timerStatus.task);
}

// The last 112 days (16 weeks), little-endian:
//   u8 version = 1, u8 daily goal hours, i32 today (days since 1970, -1 if the clock is unknown),
//   u16 count, then per day, oldest first: u16 focused minutes, u8 finished sessions
constexpr int STATS_DAYS = 112;
inline size_t statsBlob(uint8_t* out) {
  const int32_t today = clockd::today();
  size_t i = 0;
  out[i++] = 1;
  out[i++] = (uint8_t)min(255, (goalMinutes() + 30) / 60);  // whole hours; STATUS has the minutes
  memcpy(out + i, &today, 4), i += 4;
  const uint16_t count = today < 0 ? 0 : STATS_DAYS;
  memcpy(out + i, &count, 2), i += 2;
  for (int k = 0; k < count; k++) {
    const DayStat d = sys.focus.get(today - (STATS_DAYS - 1) + k);
    const uint16_t mins = (uint16_t)min(65535.0f, d.secs / 60);
    memcpy(out + i, &mins, 2), i += 2;
    out[i++] = (uint8_t)min<uint16_t>(255, d.sessions);
  }
  return i;
}

// Per task, little-endian: u8 version = 1, i32 today, u8 count, then per task in list order:
//   u8 id, then 14 x u16 focused minutes, oldest first (the last is today).
// Untagged time is the day total (STATS) minus these.
constexpr int TASK_STATS_DAYS = 14;
inline size_t taskStatsBlob(uint8_t* out) {
  const int32_t today = clockd::today();
  size_t i = 0;
  out[i++] = 1;
  memcpy(out + i, &today, 4), i += 4;
  const uint8_t count = today < 0 ? 0 : tasks.count();
  out[i++] = count;
  for (int k = 0; k < count; k++) {
    const int id = tasks.at(k).id;
    out[i++] = id;
    for (int d = 0; d < TASK_STATS_DAYS; d++) {
      const uint16_t mins = (uint16_t)min(65535.0f, sys.focus.taskSecs(today - (TASK_STATS_DAYS - 1) + d, id) / 60);
      memcpy(out + i, &mins, 2), i += 2;
    }
  }
  return i;
}

inline void publish() {
  if (!chStatus) return;
  static char json[512];
  static uint8_t blob[8 + STATS_DAYS * 3];
  static uint8_t taskBlob[6 + MAX_TASKS * (1 + TASK_STATS_DAYS * 2)];
  statusJson(json, sizeof json);
  chStatus->setValue((const uint8_t*)json, strlen(json));
  chStats->setValue(blob, statsBlob(blob));
  chTaskStats->setValue(taskBlob, taskStatsBlob(taskBlob));
  // the lists only change on a command or a button: resend them when they do
  static uint32_t tasksSent = UINT32_MAX, curSent = UINT32_MAX, messagesSent = UINT32_MAX;
  if (tasksSent != tasks.version || curSent != (uint32_t)tasks.current()) {
    const String t = "cur:" + String(tasks.current()) + "\n" + tasks.text();
    chTasks->setValue((const uint8_t*)t.c_str(), t.length());
    tasksSent = tasks.version;
    curSent = tasks.current();
  }
  if (messagesSent != messages.version) {
    const String m = messages.text();
    chMessages->setValue((const uint8_t*)m.c_str(), m.length());
    messagesSent = messages.version;
  }
}

inline void runCommand(char* c, uint32_t now) {
  if (!strncmp(c, "time:", 5)) {
    clockd::setEpoch((time_t)atoll(c + 5));
  } else if (!strncmp(c, "tz:", 3)) {
    setTimezone(c + 3);
  } else if (!strncmp(c, "key:", 4)) {
    sys.prefs.raw().putString("ai.key", c + 4);
  } else if (!strncmp(c, "model:", 6)) {
    sys.prefs.raw().putString("ai.model", c[6] ? c + 6 : DEFAULT_AI_MODEL);
  } else if (!strncmp(c, "tasks:", 6)) {
    tasks.setAll(c + 6);
  } else if (!strncmp(c, "goal:", 5)) {
    setGoalMinutes(atoi(c + 5) * 60);
  } else if (!strncmp(c, "goalmin:", 8)) {
    setGoalMinutes(atoi(c + 8));
  } else if (!strncmp(c, "msgs:", 5)) {
    messages.setAll(c + 5);
  } else if (!strcmp(c, "update:end")) {
    update::finish(now);
  } else if (!strcmp(c, "update:cancel")) {
    update::cancel(now);
  } else if (!strncmp(c, "update:", 7)) {
    update::start(c + 7, now);
    // the fast lane for it: the biggest packets the radio takes, and a connection event every 15 ms
    if (update::state == update::PREPARING)
      for (uint16_t h : NimBLEDevice::getServer()->getPeerDevices()) {
        NimBLEDevice::getServer()->setDataLen(h, 251);
        NimBLEDevice::getServer()->updateConnParams(h, 12, 12, 0, 400);
      }
  } else if (!strncmp(c, "wifi:", 5)) {
    if (char* tab = strchr(c + 5, '\t')) {
      *tab = 0;
      saveNetwork(c + 5, tab + 1, now);
    }
  }
  memset(c, 0, strlen(c));  // don't keep a key or password lying around in RAM
}

// ---------- setup and upkeep ----------

inline void begin(const char* timezone, uint32_t now) {
  setTimezone(sys.prefs.raw().getString("sys.tz", timezone).c_str(), false);
  // why a connection attempt failed (201 no network found, 15/202/204 password or security, 2/4 timeouts)
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("wifi: disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (wifiEnabled()) wifiConnect(now);
  if (btEnabled()) setBluetooth(true);
}

inline void tick(uint32_t now) {
  static bool clockSeen = false;
  static uint32_t lastPublish = 0;
  if (!clockSeen && clockd::known()) {
    clockSeen = true;
    sys.focus.flush(now);  // focus time from before the clock was known joins today
  }

  if (commandPending) {
    runCommand(command, now);
    commandPending = false;
    publish();
  }
  // and in case advertising stopped some other way: with Bluetooth on and nobody connected, be visible
  static uint32_t lastAdvCheck = 0;
  if (btOn && serviceReady && btClients == 0 && now - lastAdvCheck > 5000) {
    lastAdvCheck = now;
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) NimBLEDevice::startAdvertising();
  }
  if (btOn && btClients > 0 && now - lastPublish > 1000) {
    lastPublish = now;
    publish();
  }
  if (pairing && (int32_t)(now - pairAt) > 60000) pairing = false;  // signed: pairAt is set by the Bluetooth task and can be later than now
  if (securedAt && (int32_t)(now - securedAt) > 500 && btClients > 0) {
    securedAt = 0;
    if (sys.prefs.get("bt.gatt", 0) != GATT_LAYOUT) {
      NimBLEDevice::getServer()->sendServiceChangedIndication();
      sys.prefs.set("bt.gatt", GATT_LAYOUT);
      Serial.println("bt: told the Mac the characteristics changed");
    }
  }

  if (wstate == W_OFF || wstate == W_NO_SETUP) return;
  const bool up = WiFi.status() == WL_CONNECTED;
  if (up && wstate != W_ONLINE) {
    wstate = W_ONLINE;
    Serial.printf("wifi online, ip %s\n", WiFi.localIP().toString().c_str());
  } else if (!up && wstate == W_ONLINE) {
    wstate = W_CONNECTING;
    wifiSince = now;
  } else if (wstate == W_CONNECTING && (int32_t)(now - wifiSince) > (int32_t)CONNECT_TIMEOUT_MS) {
    wstate = W_FAILED;
    wifiSince = now;
    WiFi.disconnect();
  } else if (wstate == W_FAILED && (int32_t)(now - wifiSince) > (int32_t)RETRY_MS) {
    wifiConnect(now);
  }
}

// ---------- one line each for the Settings screen, in the pixel font's uppercase ----------

inline void wifiStatus(char* out, size_t n) {
  switch (wstate) {
    case W_OFF: snprintf(out, n, "OFF"); return;
    case W_NO_SETUP: snprintf(out, n, "NOT SET UP - USE THE MAC APP"); return;
    case W_CONNECTING: snprintf(out, n, "CONNECTING..."); return;
    case W_FAILED: snprintf(out, n, "CANNOT CONNECT, RETRYING"); return;
    case W_ONLINE: {
      String s = WiFi.SSID();
      s.toUpperCase();
      snprintf(out, n, "%s %s", s.substring(0, 10).c_str(), WiFi.localIP().toString().c_str());
      return;
    }
  }
}

inline void btStatus(char* out, size_t n) {
  String s = btName;
  s.toUpperCase();
  if (!btOn) snprintf(out, n, "OFF");
  else if (pairing) snprintf(out, n, "PAIRING...");
  else if (btClients > 0) snprintf(out, n, "CONNECTED TO THE MAC");
  else snprintf(out, n, "PAIRABLE AS %s", s.c_str());
}

// ---------- serial tests ----------

inline void scanWifi(Stream& out) {
  const bool wasOff = WiFi.getMode() == WIFI_OFF;
  if (wasOff) WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks();
  out.printf("wifi scan: %d networks\n", n);
  for (int i = 0; i < n; i++)
    out.printf("  %4d dBm  ch %2d  %s%s\n", WiFi.RSSI(i), WiFi.channel(i), WiFi.SSID(i).c_str(),
               WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "  (open)" : "");
  WiFi.scanDelete();
  if (wasOff) WiFi.mode(WIFI_OFF);
}

}  // namespace net
