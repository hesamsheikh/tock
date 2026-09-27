// update.h: new firmware from the Mac app, over Bluetooth (the protocol is in docs/bluetooth.md).
//
//   1. "update:<size> <sha256> [<deflated size>]" on COMMAND
//                                            get ready to write the other app partition; the
//                                            update screen comes up. With a deflated size, what
//                                            comes is the image deflated (raw DEFLATE, about 2/3)
//   2. chunks on UPDATE                      u32 offset, u32 FNV-1a of the bytes, the bytes; in
//                                            order. Out-of-order or damaged ones are dropped, and
//                                            the Mac goes back to `got` (read UPDATE)
//   3. "update:end" on COMMAND               once all of it is in flash: check the hash, and
//                                            restart into the new firmware
//
// Chunks wait in a small buffer in internal RAM; loop() inflates them (the ROM's tinfl) and writes
// them to flash, hashed on the way, so the image never has to fit in memory.
// The new firmware starts on probation: if it crashes before it has run for a while, the
// bootloader goes back to this one (see confirm()).

#pragma once
#include <esp_heap_caps.h>
#include <esp32/rom/miniz.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "sprites.h"
#include "system.h"

namespace update {

// PREPARING comes last so an older Mac, which doesn't know it, just waits
enum State : uint8_t { IDLE, RECEIVING, INSTALLING, DONE, FAILED, PREPARING };
enum Error : uint8_t { E_NONE, E_TOO_BIG, E_MEMORY, E_SIZE, E_HASH, E_FLASH, E_TIMEOUT, E_CANCELLED, E_DATA };

constexpr uint32_t RING = 16 * 1024;  // the Mac sends at most 12 KB past what it knows arrived
inline volatile uint8_t state = IDLE, error = E_NONE;
inline volatile uint32_t got = 0, lastData = 0, damaged = 0;  // written by the Bluetooth task
inline volatile uint32_t fed = 0;      // stream bytes taken out of the ring (loop)
inline volatile uint32_t written = 0;  // image bytes in flash (loop)
inline uint32_t size = 0, stream = 0, since = 0;  // the image; what comes over (deflated or not)
inline bool deflated = false;
inline uint8_t* ring = nullptr;  // stream byte i sits at ring[i % RING] until loop() takes it
inline uint8_t* dict = nullptr;  // inflated output, and the 32 KB window DEFLATE refers back into
inline tinfl_decompressor* inflater = nullptr;
inline uint32_t dictAt = 0;
inline bool inflated = false;
inline uint8_t hash[32];
inline mbedtls_sha256_context sha;
inline esp_ota_handle_t handle = 0;
inline bool open = false;  // handle is live
inline bool ending = false;  // "update:end" came: finish once everything is written
inline const esp_partition_t* target = nullptr;

constexpr uint32_t TIMEOUT_MS = 30000, FAILED_SHOWN_MS = 6000;

// While true, the update screen covers everything and the buttons do nothing.
inline bool showing(uint32_t now) {
  return state == PREPARING || state == RECEIVING || state == INSTALLING || state == DONE ||
         (state == FAILED && now - since < FAILED_SHOWN_MS);
}

inline void release() {
  if (open) esp_ota_abort(handle);
  open = false;
  free(ring), free(dict), free(inflater);
  ring = dict = nullptr;
  inflater = nullptr;
}

inline void fail(Error e, uint32_t now) {
  release();
  error = e;
  state = FAILED;
  since = now;
  Serial.printf("update: failed, error %d\n", e);
}

// "update:<size> <sha256 hex> [<deflated size>]". The buffers come in prepare(), from loop(),
// once the update screen has moved the frame out of internal RAM.
inline void start(const char* args, uint32_t now) {
  release();
  target = esp_ota_get_next_update_partition(nullptr);
  char* rest;
  const uint32_t n = strtoul(args, &rest, 10);
  const char* hex = rest && *rest == ' ' ? rest + 1 : nullptr;
  state = PREPARING;  // so fail() below shows on screen
  if (!target || n == 0 || n > target->size) return fail(E_TOO_BIG, now);
  if (!hex || strlen(hex) < 64) return fail(E_HASH, now);
  for (int i = 0; i < 32; i++) {
    char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
    hash[i] = (uint8_t)strtoul(b, nullptr, 16);
  }
  const uint32_t z = hex[64] == ' ' ? strtoul(hex + 65, nullptr, 10) : 0;
  size = n;
  deflated = z > 0;
  stream = deflated ? z : n;
  got = fed = written = damaged = dictAt = 0;
  ending = inflated = false;
  error = E_NONE;
  since = lastData = now;
}

inline void prepare(uint32_t now) {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  ring = (uint8_t*)heap_caps_malloc(RING, caps);
  if (deflated) {
    dict = (uint8_t*)heap_caps_malloc(TINFL_LZ_DICT_SIZE, caps);
    inflater = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor), caps);
    if (inflater) tinfl_init(inflater);
  }
  if (!ring || (deflated && (!dict || !inflater))) return fail(E_MEMORY, now);
  // erases as it goes, a sector at a time
  if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) return fail(E_FLASH, now);
  open = true;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  lastData = now;
  state = RECEIVING;
  Serial.printf("update: receiving %u bytes%s for %s, internal heap %u\n", (unsigned)stream, deflated ? " deflated" : "",
                target->label, ESP.getFreeHeap());
}

// Image bytes, into flash and the hash.
inline bool emit(const uint8_t* p, size_t n, uint32_t now) {
  mbedtls_sha256_update_ret(&sha, p, n);
  if (esp_ota_write(handle, p, n) != ESP_OK) return fail(E_FLASH, now), false;
  written = written + n;
  return true;
}

// Stream bytes from the ring: straight through, or inflated. Returns how many it took.
inline size_t take(const uint8_t* p, size_t n, uint32_t now) {
  if (!deflated) return emit(p, n, now) ? n : 0;
  if (inflated) return n;  // anything after the end of the DEFLATE stream is ignored
  size_t in = n, out = TINFL_LZ_DICT_SIZE - dictAt;
  const bool more = fed + n < stream;
  const tinfl_status st = tinfl_decompress(inflater, p, &in, dict, dict + dictAt, &out, more ? TINFL_FLAG_HAS_MORE_INPUT : 0);
  if (st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT && !more)) return fail(E_DATA, now), 0;
  if (out && !emit(dict + dictAt, out, now)) return 0;
  dictAt = (dictAt + out) & (TINFL_LZ_DICT_SIZE - 1);
  if (st == TINFL_STATUS_DONE) inflated = true;
  return in;
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
  if (off != got || off + n > stream) return;  // a gap: the Mac reads `got` and goes back
  if (off + n - fed > RING) return;             // no room yet: the same
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
  if (got != stream) return fail(E_SIZE, now);
  ending = true;
}

inline void cancel(uint32_t now) {
  if (state == RECEIVING || state == INSTALLING) fail(E_CANCELLED, now);
}

inline void tick(uint32_t now) {
  if (state == PREPARING) {
    if (now != since) prepare(now);  // a frame later: the frame has moved to PSRAM by then
  } else if (state == RECEIVING) {
    if (!ending && (int32_t)(now - lastData) > (int32_t)TIMEOUT_MS) return fail(E_TIMEOUT, now);
    // what arrived, into flash (the ring wraps, and tinfl may take a piece in several goes)
    const uint32_t upto = got;
    __sync_synchronize();
    while (fed < upto && state == RECEIVING) {
      const uint32_t at = fed % RING, n = min(upto - fed, RING - at);
      const size_t k = take(ring + at, n, now);
      if (!k) break;
      fed = fed + k;
    }
    if (state != RECEIVING || !ending || fed < stream) return;
    if (written != size) return fail(deflated ? E_DATA : E_SIZE, now);
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
// (size is the stream's: what the Mac sends)
inline size_t report(uint8_t* out) {
  const uint32_t v[3] = {got, stream, written};
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
    case PREPARING: line = "GETTING READY"; break;
    case RECEIVING:
      f = stream ? (float)fed / stream : 0;
      snprintf(buf, sizeof buf, "RECEIVING %d%%", stream ? (int)((uint64_t)fed * 100 / stream) : 0);
      line = buf;
      break;
    case INSTALLING: f = 1, line = "CHECKING IT"; break;
    case DONE: f = 1, line = "RESTARTING"; break;
    default: {
      static const char* why[] = {"", "TOO BIG FOR THE FIRE", "NOT ENOUGH MEMORY", "SOME OF IT GOT LOST",
                                  "IT DID NOT CHECK OUT", "COULD NOT WRITE IT", "THE MAC WENT QUIET", "CANCELLED",
                                  "IT DID NOT UNPACK"};
      line = why[min<int>(error, 8)];
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
