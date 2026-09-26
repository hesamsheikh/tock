// power.h: hold the power key to turn the FIRE off, and hold it again to turn it on.
//
// The FIRE's power key belongs to the IP5306 power chip (I2C 0x75), not to a GPIO.
// M5Unified configures it as "double press = off". We switch that to "long press (2 s) = off",
// which the chip then does by itself, even if the firmware has hung.
//
// The chip powers on from a press of any length, so "hold to turn on" is done in firmware:
// on battery, setup() keeps the screen dark and watches the chip's key flags. If the key was
// let go before 2 s, the firmware switches the power back off.
//
// On USB power the chip cannot cut power, so a long press only turns the screen off ("soft off"),
// and the next long press restarts the FIRE.

#pragma once
#include <M5Unified.h>
#include <esp_system.h>

namespace power {

constexpr uint8_t IP5306 = 0x75;
constexpr uint32_t I2C_FREQ = 400000;

constexpr uint8_t SYS_CTL0 = 0x00;
constexpr uint8_t CTL0_BOOST_ON = 0x20;          // 5 V boost: on battery, everything runs from it
constexpr uint8_t CTL0_KEY_OFF = 0x01;           // the key may turn the power off

constexpr uint8_t SYS_CTL1 = 0x01;
constexpr uint8_t CTL1_OFF_BY_LONG_PRESS = 0x80; // 1: long press = off, 0: double press = off
constexpr uint8_t CTL1_LIGHT_BY_DOUBLE = 0x40;   // flashlight on double press (the FIRE has none)

constexpr uint8_t READ0 = 0x70;
constexpr uint8_t READ0_VIN = 0x08;              // charger active: USB power is present

constexpr uint8_t KEY_FLAGS = 0x77;              // latched key events, write 1 to clear
constexpr uint8_t KEY_SHORT = 0x01, KEY_LONG = 0x02, KEY_DOUBLE = 0x04;

constexpr uint32_t LONG_PRESS_MS = 2000;         // SYS_CTL2 bit 4 = 0, as M5Unified leaves it

inline uint8_t readReg(uint8_t reg) { return M5.In_I2C.readRegister8(IP5306, reg, I2C_FREQ); }
inline void writeReg(uint8_t reg, uint8_t v) { M5.In_I2C.writeRegister8(IP5306, reg, v, I2C_FREQ); }
inline void clearKeys() { writeReg(KEY_FLAGS, KEY_SHORT | KEY_LONG | KEY_DOUBLE); }
inline void setKeyOff(bool on) {
  const uint8_t v = readReg(SYS_CTL0);
  writeReg(SYS_CTL0, on ? (v | CTL0_KEY_OFF) : (v & ~CTL0_KEY_OFF));
}

inline bool onUsb() { return readReg(READ0) & READ0_VIN; }

// True once for each long press of the power key.
inline bool longPressed() {
  const uint8_t f = readReg(KEY_FLAGS) & (KEY_SHORT | KEY_LONG | KEY_DOUBLE);
  if (!f) return false;
  clearKeys();
  Serial.printf("power key: %s%s%s\n", f & KEY_SHORT ? "short " : "", f & KEY_LONG ? "long " : "", f & KEY_DOUBLE ? "double" : "");
  return f & KEY_LONG;
}

// Cut the power. On battery the boost stops and the FIRE goes dark; on USB this cannot work,
// so fall back to deep sleep.
[[noreturn]] inline void shutdown() {
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  clearKeys();
  setKeyOff(true);
  writeReg(SYS_CTL0, readReg(SYS_CTL0) & ~CTL0_BOOST_ON);
  delay(300);
  M5.Power.powerOff();
  for (;;) delay(1000);
}

inline uint32_t keyOffAt = 0;  // when to hand the key back to the chip after a hold-to-start

// Call right after M5.begin(), before anything lights the screen.
inline void begin() {
  writeReg(SYS_CTL1, readReg(SYS_CTL1) | CTL1_OFF_BY_LONG_PRESS | CTL1_LIGHT_BY_DOUBLE);
  // on USB, or restarting itself (after an update): no key press to wait for
  if (onUsb() || esp_reset_reason() == ESP_RST_SW) {
    setKeyOff(true);
    return;
  }

  // On battery: the press that woke us may still be going. Keep the chip from treating it as
  // "off", and watch its key flags. Booting already used part of the 2 s.
  //   long-press flag   the key was held: start
  //   short-press flag  the key was let go too soon: switch back off
  //   neither           the flags told us nothing: start anyway, so the FIRE can never lock itself out
  M5.Display.setBrightness(0);
  setKeyOff(false);
  const uint8_t early = readReg(KEY_FLAGS);
  clearKeys();
  if ((early & KEY_SHORT) && !(early & KEY_LONG)) shutdown();  // a tap that ended while we booted
  const uint32_t start = millis();
  while (millis() - start < LONG_PRESS_MS + 500) {
    const uint8_t f = readReg(KEY_FLAGS);
    if (f & KEY_LONG) break;
    if (f & KEY_SHORT) shutdown();
    delay(20);
  }
  clearKeys();
  keyOffAt = millis() + 1500;  // give the finger time to let go before a long press means off again
}

// Call from loop(). Returns true while the FIRE is soft-off (USB only): draw nothing then.
inline bool update() {
  static bool softOff = false;
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  if (keyOffAt && now >= keyOffAt) {
    keyOffAt = 0;
    clearKeys();
    setKeyOff(true);
  }
  if (now - lastPoll < 100) return softOff;
  lastPoll = now;

  // On battery the chip itself turns off on a long press; we only clear the flags so a stale
  // tap is never mistaken for "let go too soon" at the next power-on.
  const bool longPress = longPressed();
  if (!onUsb() || !longPress) return softOff;
  if (softOff) esp_restart();  // "power on" again
  softOff = true;
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  M5.Speaker.stop();
  return softOff;
}

inline void printRegisters(Stream& out) {
  out.printf("ip5306 ctl0=%02x ctl1=%02x ctl2=%02x read0=%02x keys=%02x usb=%d\n",
             readReg(SYS_CTL0), readReg(SYS_CTL1), readReg(0x02), readReg(READ0), readReg(KEY_FLAGS), onUsb());
}

}  // namespace power
