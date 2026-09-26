// voice.h: Talk's speaker output. Plays 24 kHz speech through the FIRE's 8-bit DAC (GPIO 25),
// driving the I2S port directly instead of going through M5.Speaker.
//
// Why not M5.Speaker: the DAC can only output 0..255, so a speech wave (which swings both ways)
// has to ride on a DC offset. M5Unified keeps that offset at the current peak: it snaps up the
// instant a louder sample arrives and sags during quiet parts. For beeps that is fine, but speech
// is full of pauses, so the offset keeps sagging and then jumping, and every jump is a click the
// speaker plays: the crackle at the start of words after a pause. It also stops and restarts
// output whenever its queue runs dry, which clicks again.
//
// Here the offset is planned ahead: this task looks 60 ms into the queued speech and raises the
// offset gently before a loud part arrives, then lets it settle slowly in pauses. A low offset in
// silence also keeps out supply noise (Wi-Fi bursts), which the DAC passes on in proportion to its
// output level. When the queue runs dry it keeps playing silence rather than stopping.
// The 24 kHz input is doubled to 48 kHz by interpolation, and rounding to 8 bits carries its
// error forward (noise shaping), which pushes the rounding hiss up where the speaker can't play it.
//
// The mic uses the same I2S port: end() before mic::begin(), begin() after mic::end().
// M5.Speaker must be stopped (M5.Speaker.end()) while this runs.

#pragma once
#include <driver/dac.h>
#include <driver/i2s.h>

namespace voice {

constexpr i2s_port_t PORT = I2S_NUM_0;
constexpr uint32_t IN_RATE = 24000, OUT_RATE = IN_RATE * 2;
constexpr size_t RING = IN_RATE * 2;  // up to 2 s queued, in PSRAM
constexpr int BLOCK = IN_RATE / 200;  // the task works 5 ms at a time
constexpr int LOOK = BLOCK * 12;      // and plans the offset 60 ms ahead
constexpr float RISE = 16;            // offset rise per block, in DAC steps (0 to mid in 40 ms)
constexpr float FALL = 0.03f;         // share of the gap closed per block when it can come down

inline int16_t* ring = nullptr;
inline volatile size_t head = 0, tail = 0;  // the caller pushes at head, the task plays from tail
inline volatile size_t played = 0;          // samples of speech played so far
inline volatile bool running = false, cutting = false, exited = true;
inline float bias = 0;  // DAC offset in steps (0..128)

inline size_t queued() { return head - tail; }
inline size_t space() { return RING - (head - tail); }

// Queue speech; returns how many samples fit.
inline size_t push(const int16_t* s, size_t n) {
  n = min(n, space());
  size_t h = head;
  for (size_t i = 0; i < n; i++) ring[h++ % RING] = s[i];
  head = h;
  return n;
}

// Stop what is playing now (with a 5 ms fade, so the cut doesn't click).
inline void cut() {
  if (running) cutting = true;
  else tail = head;
}

inline void task(void*) {
  static int16_t in[BLOCK];
  static uint32_t out[BLOCK * 2];  // both DAC channels per word; only GPIO 25's is enabled
  int32_t err = 0, prev = 0;
  bool leaving = false;
  for (;;) {
    if (!running) leaving = true;
    // this block: queued speech, or silence if there is none
    const size_t t = tail, avail = leaving ? 0 : head - t;
    const int n = min<size_t>(avail, BLOCK);
    for (int i = 0; i < n; i++) in[i] = ring[(t + i) % RING];
    for (int i = n; i < BLOCK; i++) in[i] = 0;
    float gain = 1, gainStep = 0;
    const bool cut = cutting;
    if (cut) gainStep = -1.0f / BLOCK;

    // the offset this block has to reach: the loudest sample coming up soon, in DAC steps
    int32_t peak = 0;
    const size_t look = min<size_t>(avail, LOOK);
    if (!cut)
      for (size_t i = 0; i < look; i++) peak = max<int32_t>(peak, abs(ring[(t + i) % RING]));
    const float target = leaving ? 0 : min(128.0f, peak / 256.0f * 1.05f + 2);
    const float from = bias;
    if (target > bias) bias = min(target, bias + RISE);
    else bias -= (bias - target) * (leaving ? 0.25f : FALL);
    if (leaving && bias < 0.5f) bias = 0;

    // 24 kHz to 48 kHz, offset gliding across the block, rounded to 8 bits with noise shaping
    for (int i = 0; i < BLOCK; i++) {
      const int32_t cur = (int32_t)(in[i] * gain);
      gain += gainStep;
      for (int k = 0; k < 2; k++) {
        const int32_t s = k ? cur : (prev + cur) / 2;
        const float b = from + (bias - from) * (i * 2 + k + 1) / (BLOCK * 2);
        const int32_t v = (int32_t)(b * 256) + s + err;
        const int32_t u = constrain((v + 128) >> 8, 0, 255);
        err = constrain(v - (u << 8), -256, 256);
        out[i * 2 + k] = (uint32_t)u << 24 | (uint32_t)u << 8;
      }
      prev = cur;
    }
    if (cut) {
      tail = head;
      cutting = false;
      prev = 0;
    } else {
      tail = t + n;
      played = played + n;
    }
    size_t wrote;
    i2s_write(PORT, out, sizeof out, &wrote, portMAX_DELAY);
    if (leaving && bias == 0) break;
  }
  exited = true;
  vTaskDelete(nullptr);
}

inline bool begin() {
  if (running) return true;
  if (!ring) ring = (int16_t*)ps_malloc(RING * 2);
  if (!ring) return false;
  i2s_config_t c = {};
  c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  c.sample_rate = OUT_RATE;
  c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  c.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  c.dma_buf_count = 6;
  c.dma_buf_len = BLOCK * 2;  // 30 ms in flight
  c.use_apll = false;
  c.tx_desc_auto_clear = false;  // if the task were ever late, repeat rather than drop to 0 (a pop)
  if (i2s_driver_install(PORT, &c, 0, nullptr) != ESP_OK) return false;
  i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);  // GPIO 25, the speaker
  i2s_zero_dma_buffer(PORT);
  head = tail = 0;
  bias = 0;
  cutting = false;
  running = true;
  exited = false;
  // on the loop's core, above it: playback never waits on drawing (the network runs on core 0)
  xTaskCreatePinnedToCore(task, "voice", 3072, nullptr, 5, nullptr, 1);
  return true;
}

// Glides the offset down to 0 first, so stopping makes no pop.
inline void end() {
  if (!running) return;
  running = false;
  for (uint32_t t0 = millis(); !exited && millis() - t0 < 500;) delay(2);  // the glide takes ~0.1 s
  if (!exited) Serial.println("voice: output task did not stop");
  i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
  i2s_driver_uninstall(PORT);
  dac_output_disable(DAC_CHANNEL_1);
  pinMode(25, OUTPUT);
  digitalWrite(25, LOW);  // as M5.Speaker leaves it: quiet
  head = tail = 0;
}

}  // namespace voice
