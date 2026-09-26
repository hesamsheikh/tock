// update.h: new firmware from the Mac app, over Bluetooth (the protocol is in docs/bluetooth.md).
//
//   1. "update:<size> <sha256>" on COMMAND   start writing to the other app partition; the update
//                                            screen comes up
//   2. chunks on UPDATE                      u32 offset, u32 FNV-1a of the bytes, the bytes; in
//                                            order. Out-of-order or damaged ones are dropped, and
//                                            the Mac goes back to `got` (read UPDATE)
//   3. "update:end" on COMMAND               once all of it is in flash: check the hash, and
//                                            restart into the new firmware
//
// Chunks wait in a small buffer in internal RAM and go to flash from loop(), hashed on the way.
// Not through PSRAM: an image parked there came back with bytes changed.
// The new firmware starts on probation: if it crashes before it has run for a while, the
// bootloader goes back to this one (see confirm()).

#pragma once
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "sprites.h"
#include "system.h"

namespace update {

enum State : uint8_t { IDLE, RECEIVING, INSTALLING, DONE, FAILED };
enum Error : uint8_t { E_NONE, E_TOO_BIG, E_MEMORY, E_SIZE, E_HASH, E_FLASH, E_TIMEOUT, E_CANCELLED };

constexpr uint32_t RING = 16 * 1024;  // the Mac sends at most 12 KB past what it knows arrived
inline volatile uint8_t state = IDLE, error = E_NONE;
inline volatile uint32_t got = 0, lastData = 0, damaged = 0;  // written by the Bluetooth task
inline volatile uint32_t written = 0;                          // what's in flash (loop)
inline uint32_t size = 0, since = 0;
inline uint8_t* ring = nullptr;  // byte i of the image sits at ring[i % RING] until it's written
inline uint8_t hash[32];
inline mbedtls_sha256_context sha;
inline esp_ota_handle_t handle = 0;
inline bool open = false;  // handle is live
inline bool ending = false;  // "update:end" came: finish once everything is written
inline const esp_partition_t* target = nullptr;

constexpr uint32_t TIMEOUT_MS = 30000, FAILED_SHOWN_MS = 6000;

// While true, the update screen covers everything and the buttons do nothing.
inline bool showing(uint32_t now) {
  return state == RECEIVING || state == INSTALLING || state == DONE ||
         (state == FAILED && now - since < FAILED_SHOWN_MS);
}

inline void release() {
  if (open) esp_ota_abort(handle);
  open = false;
  if (ring) free(ring);
  ring = nullptr;
}

inline void fail(Error e, uint32_t now) {
  release();
  error = e;
  state = FAILED;
  since = now;
  Serial.printf("update: failed, error %d\n", e);
}

// "update:<size> <sha256 hex>"
inline void start(const char* args, uint32_t now) {
  release();
  target = esp_ota_get_next_update_partition(nullptr);
  const uint32_t n = strtoul(args, nullptr, 10);
  const char* hex = strchr(args, ' ');
  state = RECEIVING;  // so fail() below shows on screen
  if (!target || n == 0 || n > target->size) return fail(E_TOO_BIG, now);
  if (!hex || strlen(hex + 1) != 64) return fail(E_HASH, now);
  for (int i = 0; i < 32; i++) {
    char b[3] = {hex[1 + i * 2], hex[2 + i * 2], 0};
    hash[i] = (uint8_t)strtoul(b, nullptr, 16);
  }
  ring = (uint8_t*)heap_caps_malloc(RING, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ring) return fail(E_MEMORY, now);
  // erases as it goes, a sector at a time
  if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) return fail(E_FLASH, now);
  open = true;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  size = n;
  got = written = damaged = 0;
  ending = false;
  error = E_NONE;
  since = lastData = now;
  Serial.printf("update: receiving %u bytes for %s\n", (unsigned)n, target->label);
}

inline uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

// From the Bluetooth task: one chunk, u32 offset and u32 FNV-1a (little-endian), then the bytes.
inline void chunk(const uint8_t* d, size_t n) {
  if (state != RECEIVING || !ring || ending || n <= 8) return;
  uint32_t off, sum;
  memcpy(&off, d, 4);
  memcpy(&sum, d + 4, 4);
  n -= 8;
  if (off != got || off + n > size) return;  // a gap: the Mac reads `got` and goes back
  if (off + n - written > RING) return;       // no room yet: the same
  if (fnv1a(d + 8, n) != sum) return (void)damaged++;
  const uint32_t at = off % RING, first = min<uint32_t>(n, RING - at);
  memcpy(ring + at, d + 8, first);
  memcpy(ring, d + 8 + first, n - first);
  __sync_synchronize();  // the bytes are in before loop() sees `got` move
  got = off + n;
  lastData = millis();
}

// "update:end": finish once the last bytes are in flash (tick()).
inline void finish(uint32_t now) {
  if (state != RECEIVING) return;
  if (got != size) return fail(E_SIZE, now);
  ending = true;
}

inline void cancel(uint32_t now) {
  if (state == RECEIVING || state == INSTALLING) fail(E_CANCELLED, now);
}

inline void tick(uint32_t now) {
  if (state == RECEIVING) {
    if (!ending && (int32_t)(now - lastData) > (int32_t)TIMEOUT_MS) return fail(E_TIMEOUT, now);
    // what arrived, into flash, in at most two pieces (the ring wraps)
    const uint32_t upto = got;
    __sync_synchronize();
    while (written < upto) {
      const uint32_t at = written % RING, n = min(upto - written, RING - at);
      mbedtls_sha256_update_ret(&sha, ring + at, n);
      if (esp_ota_write(handle, ring + at, n) != ESP_OK) return fail(E_FLASH, now);
      written = written + n;
    }
    if (!ending || written < size) return;
    state = INSTALLING;
    since = now;
    Serial.printf("update: all %u bytes in flash, %u chunks came in damaged\n", (unsigned)size, (unsigned)damaged);
  } else if (state == INSTALLING) {
    uint8_t h[32];
    mbedtls_sha256_finish_ret(&sha, h);
    mbedtls_sha256_free(&sha);
    if (memcmp(h, hash, 32)) return fail(E_HASH, now);
    // esp_ota_end checks the image in flash (its header and its own hash) before it can boot
    open = false;
    if (esp_ota_end(handle) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK) return fail(E_FLASH, now);
    release();
    state = DONE;
    since = now;
    Serial.println("update: installed, restarting");
  } else if (state == DONE && now - since > 2000) {
    ESP.restart();
  }
}

// What the Mac reads on UPDATE: u8 state, u8 error, u32 got, u32 size, u32 written.
inline size_t report(uint8_t* out) {
  const uint32_t v[3] = {got, size, written};
  out[0] = state;
  out[1] = error;
  memcpy(out + 2, v, 12);
  return 14;
}

// A new firmware runs on probation until this: call it once the FIRE has been up for a while.
// Until then a crash or a reset makes the bootloader go back to the previous one.
inline void confirm() {
  esp_ota_img_states_t s;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("update: new firmware confirmed");
  }
}

// The update screen.
inline void draw(Gfx& g, uint32_t now) {
  g.fillScreen(pal::bg);
  const int t = now / 400;
  const bool ok = state != FAILED;
  const TockPose pose = !ok ? POSE_SIT : state == DONE ? POSE_CHEER : t % 2 ? POSE_TICK : POSE_TOCK;
  drawTock(g, still(pose), (SCREEN_W - TOCK_W * 6) / 2, 118, 6);
  g.drawText(state == FAILED ? "UPDATE FAILED" : state == DONE ? "UPDATED" : "UPDATING", SCREEN_W / 2, 136, 3,
             ok ? pal::body : pal::red, CENTER);

  constexpr int X = 40, Y = 172, W = SCREEN_W - 80, H = 12;
  float f = 0;
  const char* line = "";
  char buf[40];
  switch (state) {
    case RECEIVING:
      f = size ? (float)written / size : 0;
      snprintf(buf, sizeof buf, "RECEIVING %d%%", size ? (int)((uint64_t)written * 100 / size) : 0);
      line = buf;
      break;
    case INSTALLING: f = 1, line = "CHECKING IT"; break;
    case DONE: f = 1, line = "RESTARTING"; break;
    default: {
      static const char* why[] = {"", "TOO BIG FOR THE FIRE", "NOT ENOUGH MEMORY", "SOME OF IT GOT LOST",
                                  "IT DID NOT CHECK OUT", "COULD NOT WRITE IT", "THE MAC WENT QUIET", "CANCELLED"};
      line = why[min<int>(error, 7)];
      snprintf(buf, sizeof buf, "%s - STILL ON %s", line, TOCK_VERSION);
      line = buf;
    }
  }
  if (ok) {
    g.fillRect(X - 2, Y - 2, W + 4, H + 4, pal::grey);
    g.fillRect(X, Y, W, H, pal::bg);
    g.fillRect(X, Y, (int)(W * f), H, pal::body);
  }
  g.drawText(line, SCREEN_W / 2, ok ? Y + 26 : Y + 8, 2, pal::grey, CENTER);
}

}  // namespace update
