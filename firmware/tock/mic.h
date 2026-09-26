// mic.h: the FIRE's microphone (on the M5GO base, analog on GPIO 34), read through the ESP32's
// I2S peripheral in built-in-ADC mode. M5Unified's ADC mic path delivers no samples on this
// ESP32 core, so Tock drives it directly with the classic I2S driver.
//
// The speaker uses the same I2S port: call M5.Speaker.end() before begin(), and
// M5.Speaker.begin() after end().

#pragma once
#include <driver/adc.h>
#include <driver/i2s.h>

namespace mic {

constexpr i2s_port_t PORT = I2S_NUM_0;
constexpr int GAIN = 12;  // the FIRE's mic is quiet; this brings speech to a useful level

inline bool running = false;
inline int32_t dc = 2048 << 8;  // running average of the raw signal (fixed point), removed as DC

inline bool begin(uint32_t rate) {
  if (running) return true;
  i2s_config_t c = {};
  c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN);
  c.sample_rate = rate;
  c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  c.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  c.dma_buf_count = 8;
  c.dma_buf_len = 512;
  c.use_apll = false;
  if (i2s_driver_install(PORT, &c, 0, nullptr) != ESP_OK) return false;
  i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_6);  // GPIO 34
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
  i2s_adc_enable(PORT);
  running = true;
  return true;
}

inline void end() {
  if (!running) return;
  i2s_adc_disable(PORT);
  i2s_driver_uninstall(PORT);
  running = false;
}

// Whatever has been captured since the last call (never waits), as signed 16-bit PCM.
inline size_t read(int16_t* out, size_t max) {
  if (!running || max < 2) return 0;
  size_t bytes = 0;
  i2s_read(PORT, out, (max & ~1u) * 2, &bytes, 0);
  const size_t n = bytes / 2;
  for (size_t i = 0; i + 1 < n; i += 2) {  // the ADC mode hands samples over in swapped pairs
    const int16_t t = out[i];
    out[i] = out[i + 1];
    out[i + 1] = t;
  }
  for (size_t i = 0; i < n; i++) {
    const int32_t v = (out[i] & 0x0fff) << 8;  // 12-bit reading; the top bits are the channel
    dc += (v - dc) >> 10;
    const int32_t s = ((v - dc) >> 8) * 16 * GAIN / 4;
    out[i] = (int16_t)constrain(s, -32767, 32767);
  }
  return n;
}

}  // namespace mic
