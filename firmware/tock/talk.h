// talk.h: the Talk app. A voice conversation with OpenAI, straight from the FIRE over Wi-Fi (the
// Mac only hands over the API key and model, see net.h). Shown as a pixel orb in Tock's mustard.
//
// A walkie-talkie: hold A and talk, let go to send. Tock thinks, answers, and waits for the next
// message. Holding A while Tock speaks cuts it off and records your reply.
//   hold A  record, let go to send     B  stop Tock's reply     (hold B: home)
//
// The FIRE's mic and speaker share one I2S port, so Tock listens or speaks, never both. Replies
// play through voice.h rather than M5.Speaker (which clicks in speech pauses, see there), so Talk
// is a quiet screen: no beeps while M5.Speaker is off.
// Audio is 24 kHz, 16-bit mono PCM both ways. Two protocols, picked by the model name:
//   Realtime API (gpt-realtime-*)  wss://api.openai.com/v1/realtime?model=...
//     turn detection off; on release: input_audio_buffer.commit + response.create; replies as
//     response.output_audio.delta, ended by response.done
//   GPT-Live (gpt-live-*)          wss://api.openai.com/v1/live/sessions
//     session.start with a Responses backend; unmuted only while A is held (muting ends the turn;
//     if GPT-Live stays quiet, Tock streams silence instead). Replies as session.output_audio.delta,
//     with no "done" event, so a reply ends after a quiet gap. Billed per session minute: Tock
//     hangs up after a minute idle, and calls back when you next hold A.
//
// All networking runs in its own task on core 0 (TalkLink), so the screen and buttons never wait
// on a slow network write.

#pragma once
#include <WiFiClientSecure.h>
#include "ca.h"
#include "mbedtls/base64.h"
#include "mic.h"
#include "net.h"
#include "sprites.h"
#include "system.h"
#include "voice.h"

// ---------- a small WebSocket client over TLS ----------
// Hand-rolled rather than a library: replies can be large, and messages are kept in PSRAM.

class Wss {
 public:
  // Returns "" on success, or a short reason for the screen.
  String connect(const char* host, const String& path, const String& header) {
    close();
    tls.setCACert(OPENAI_ROOT_CA);
    tls.setHandshakeTimeout(20);
    Serial.printf("talk: connecting, heap %u (largest block %u)\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    // The TLS handshake's key exchange runs seconds in one go, on core 0, without yielding: the idle
    // task there can't feed the task watchdog meanwhile, so it's off for the handshake.
    const uint32_t t0 = millis();
    disableCore0WDT();
    const bool ok = tls.connect(host, 443);
    enableCore0WDT();
    Serial.printf("talk: tls handshake %s in %lu ms\n", ok ? "done" : "failed", (unsigned long)(millis() - t0));
    if (!ok) {
      char why[100];
      tls.lastError(why, sizeof why);
      Serial.printf("talk: tls failed: %s\n", why);
      return "CANNOT REACH OPENAI";
    }
    uint8_t nonce[16];
    for (auto& b : nonce) b = esp_random();
    char key[32];
    size_t klen = 0;
    mbedtls_base64_encode((unsigned char*)key, sizeof key, &klen, nonce, sizeof nonce);
    key[klen] = 0;
    String req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
                 "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n" + header + "\r\n\r\n";
    tls.print(req);
    req = "";  // it held the key
    const String status = readLine(8000);
    const int code = status.length() > 12 ? status.substring(9, 12).toInt() : 0;
    Serial.printf("talk: handshake reply \"%s\"\n", status.c_str());
    while (tls.connected()) {  // skip the response headers
      const String line = readLine(8000);
      if (line.length() <= 1) break;
    }
    if (code != 101) {
      tls.stop();
      return code == 401 ? "THE API KEY WAS REFUSED" : code == 404 ? "UNKNOWN MODEL" : code == 429 ? "RATE LIMITED OR NO CREDIT"
           : code ? "OPENAI SAID " + String(code) : "NO ANSWER FROM OPENAI";
    }
    open = true;
    inFrame = false;
    msgLen = 0;
    return "";
  }

  bool isOpen() { return open && tls.connected(); }

  // Give back the message buffer (PSRAM); it grows again when needed.
  void release() {
    free(msg);
    msg = nullptr;
    cap = msgLen = 0;
  }

  void close() {
    if (open) sendFrame(0x8, nullptr, 0);
    tls.stop();
    open = false;
  }

  bool sendText(const char* s, size_t n) { return sendFrame(0x1, (const uint8_t*)s, n); }

  // Reads whatever has arrived. Returns a whole text message (NUL-terminated) when one completes.
  char* poll(size_t& len) {
    while (open && tls.available() > 0) {
      if (!inFrame) {
        uint8_t h[2];
        if (!readExact(h, 2)) return nullptr;
        fin = h[0] & 0x80;
        const uint8_t op = h[0] & 0x0f;
        uint64_t n = h[1] & 0x7f;
        if (n == 126 || n == 127) {
          uint8_t e[8];
          const int k = n == 126 ? 2 : 8;
          if (!readExact(e, k)) return nullptr;
          n = 0;
          for (int i = 0; i < k; i++) n = n << 8 | e[i];
        }
        if (h[1] & 0x80) {  // servers don't mask, but skip it if one does
          uint8_t m[4];
          readExact(m, 4);
        }
        frameLeft = n;
        control = op >= 0x8;
        if (!control && op != 0) msgLen = 0;  // a new message (0 continues the last one)
        if (control) ctlOp = op, ctlLen = 0;
        inFrame = true;
      }
      size_t take = min<size_t>(frameLeft, tls.available());
      // count what read() really hands over: it can be less than available() said, and counting
      // the rest would lose the frame boundaries for the whole connection
      if (control) {
        uint8_t skip[64];
        const size_t room = sizeof ctl - ctlLen;
        const int r = room ? tls.read(ctl + ctlLen, min(take, room)) : tls.read(skip, min(take, sizeof skip));
        if (r <= 0) break;
        take = r;
        if (room) ctlLen += r;
      } else {
        if (!reserve(msgLen + take + 1)) {
          close();
          return nullptr;
        }
        const int r = tls.read(msg + msgLen, take);
        if (r <= 0) break;
        take = r;
        msgLen += r;
      }
      frameLeft -= take;
      if (frameLeft) continue;
      inFrame = false;
      if (control) {
        if (ctlOp == 0x9) sendFrame(0xA, ctl, ctlLen);  // ping: pong
        if (ctlOp == 0x8) open = false, tls.stop();     // closed by OpenAI
      } else if (fin) {
        msg[msgLen] = 0;
        len = msgLen;
        return (char*)msg;
      }
    }
    return nullptr;
  }

 private:
  WiFiClientSecure tls;
  bool open = false, inFrame = false, fin = false, control = false;
  uint64_t frameLeft = 0;
  uint8_t* msg = nullptr;
  size_t cap = 0, msgLen = 0;
  uint8_t ctl[128], ctlOp = 0;
  size_t ctlLen = 0;

  bool reserve(size_t n) {
    if (n <= cap) return true;
    if (n > 2 * 1024 * 1024) return false;
    const size_t want = max(n, cap ? cap * 2 : (size_t)64 * 1024);
    uint8_t* p = (uint8_t*)ps_realloc(msg, want);
    if (!p) return false;
    msg = p, cap = want;
    return true;
  }

  // The waits below sleep a tick between tries: this runs on core 0, and spinning there for more
  // than 5 s starves its idle task, which the task watchdog answers with a reset.
  bool readExact(uint8_t* p, size_t n) {
    const uint32_t start = millis();
    while (n) {
      const int r = tls.read(p, n);
      if (r > 0) p += r, n -= r;
      else if (millis() - start > 3000 || !tls.connected()) return false;
      else vTaskDelay(1);
    }
    return true;
  }

  // One line of the HTTP reply, without the '\n' ("" on timeout).
  String readLine(uint32_t timeoutMs) {
    String line;
    const uint32_t start = millis();
    while (millis() - start < timeoutMs && tls.connected()) {
      const int c = tls.read();
      if (c < 0) {
        vTaskDelay(1);
        continue;
      }
      if (c == '\n') return line;
      line += (char)c;
    }
    return line;
  }

  bool sendFrame(uint8_t op, const uint8_t* data, size_t len) {
    uint8_t h[14];
    size_t hl = 0;
    h[hl++] = 0x80 | op;
    if (len < 126) h[hl++] = 0x80 | len;
    else if (len < 65536) h[hl++] = 0x80 | 126, h[hl++] = len >> 8, h[hl++] = len & 0xff;
    else {
      h[hl++] = 0x80 | 127;
      for (int i = 7; i >= 0; i--) h[hl++] = (uint64_t)len >> (8 * i);
    }
    const uint32_t mk = esp_random();
    uint8_t* mask = h + hl;
    memcpy(mask, &mk, 4);
    hl += 4;
    if (tls.write(h, hl) != hl) return false;
    uint8_t tmp[512];
    for (size_t off = 0; off < len; off += sizeof tmp) {
      const size_t n = min(sizeof tmp, len - off);
      for (size_t i = 0; i < n; i++) tmp[i] = data[off + i] ^ mask[(off + i) & 3];
      if (tls.write(tmp, n) != n) return false;
    }
    return true;
  }
};

// ---------- the network task ----------
// Owns the WebSocket. The loop hands it mic audio and control messages; it hands back reply audio
// (a ring the loop plays from) and a few events.

struct TalkEvent {
  uint8_t kind;
  char text[100];
};
enum { EV_READY = 1, EV_FAIL, EV_CLOSED, EV_DONE, EV_HEARD };

class TalkLink {
 public:
  static constexpr int RATE = 24000, CHUNK = 2400;  // mic audio goes out 100 ms at a time
  static constexpr size_t RING = RATE * 40;          // 40 s of reply, in PSRAM
  static constexpr size_t MIC_RING = RATE * 4;
  static constexpr int ENV_BLOCK = 1200, ENV_COUNT = RING / ENV_BLOCK;  // loudness per 50 ms, for the orb

  int16_t* ring = nullptr;  // reply audio: the task writes at head, the loop plays from tail
  volatile size_t head = 0, tail = 0;
  uint8_t env[ENV_COUNT] = {};
  volatile uint32_t lastDeltaAt = 0;
  volatile bool discarding = false;  // skip the rest of a reply the user stopped

  void begin() {
    if (running) return;
    if (!ring) ring = (int16_t*)ps_malloc(RING * 2);
    if (!mic) mic = (int16_t*)ps_malloc(MIC_RING * 2);
    if (!out) out = (char*)ps_malloc(CHUNK * 3 + 128);
    if (!ctlQ) ctlQ = xQueueCreate(12, sizeof(char*));
    if (!evQ) evQ = xQueueCreate(8, sizeof(TalkEvent));
    quitReq = false;
    running = true;
    xTaskCreatePinnedToCore(run, "talknet", 16384, this, 3, nullptr, 0);
  }

  // Leaving Talk: the task stops and everything goes back, so the frame can have its internal
  // RAM again (the task's stack sits where the frame was). If the task is stuck in a handshake,
  // it stops when that ends, and its buffers stay for next time rather than go while in use.
  void end() {
    if (!running) return;
    quitReq = true;
    for (int i = 0; i < 100 && running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (running) return;
    free(ring), free(mic), free(out);
    ring = mic = nullptr;
    out = nullptr;
    vQueueDelete(ctlQ), vQueueDelete(evQ);
    ctlQ = evQ = nullptr;
    ws.release();
  }

  void connect(bool gptLive, const String& model, const String& path, const String& auth, const String& start) {
    live = gptLive;
    modelName = model;
    modelName.toUpperCase();
    connPath = path;
    connAuth = auth;
    startJson = start;
    closeReq = false;
    connectReq = true;
  }
  void close() { closeReq = true; }
  bool isOpen() const { return open; }

  void send(const char* json) {
    if (!ctlQ) return;
    char* p = strdup(json);
    if (p && xQueueSend(ctlQ, &p, 0) != pdTRUE) free(p);
  }
  bool poll(TalkEvent& e) { return evQ && xQueueReceive(evQ, &e, 0) == pdTRUE; }

  size_t available() const { return head - tail; }
  void dropPlayback() { tail = head; }

  volatile bool silence = false;  // GPT-Live fallback: stream 100 ms of silence every 100 ms

  void pushMic(const int16_t* s, size_t n) {
    for (size_t i = 0; i < n && micHead - micTail < MIC_RING; i++) mic[micHead++ % MIC_RING] = s[i];
  }
  void flushMic() { micFlush = true; }

 private:
  Wss ws;
  QueueHandle_t ctlQ = nullptr, evQ = nullptr;
  int16_t* mic = nullptr;
  volatile size_t micHead = 0, micTail = 0;
  volatile bool micFlush = false;
  char* out = nullptr;
  volatile bool connectReq = false, closeReq = false, open = false, quitReq = false, running = false;
  bool live = false;
  String modelName, connPath, connAuth, startJson;
  uint32_t heardAt = 0, silenceAt = 0;

  static void run(void* self) { ((TalkLink*)self)->loop(); }

  void post(uint8_t kind, const char* text = "") {
    TalkEvent e;
    e.kind = kind;
    snprintf(e.text, sizeof e.text, "%s", text);
    xQueueSend(evQ, &e, 0);
  }

  void loop() {
    for (;;) {
      if (quitReq) {
        if (open) ws.close();
        open = false;
        char* p;
        while (xQueueReceive(ctlQ, &p, 0) == pdTRUE) free(p);
        running = false;
        vTaskDelete(nullptr);
      }
      if (connectReq) {
        connectReq = false;
        doConnect();
      }
      if (closeReq) {
        closeReq = false;
        if (open) ws.close();
        open = false;
        char* p;
        while (xQueueReceive(ctlQ, &p, 0) == pdTRUE) free(p);
      }
      if (open) {
        if (!ws.isOpen()) {
          open = false;
          post(EV_CLOSED);
        } else {
          char* p;
          while (xQueueReceive(ctlQ, &p, 0) == pdTRUE) {
            ws.sendText(p, strlen(p));
            free(p);
          }
          if (micFlush) micTail = micHead, micFlush = false;
          while (micHead - micTail >= CHUNK) sendMic();
          if (silence && millis() - silenceAt >= 100) {
            silenceAt = millis();
            sendSilence();
          }
          size_t len;
          for (int budget = 16; budget > 0; budget--) {
            char* m = ws.poll(len);
            if (!m) break;
            onMessage(m);
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(4));
    }
  }

  void doConnect() {
    if (open) ws.close();
    open = false;
    String err;
    for (int attempt = 1; attempt <= 3; attempt++) {  // a first handshake sometimes comes back empty
      err = ws.connect("api.openai.com", connPath, connAuth);
      if (!err.length()) break;
      Serial.printf("talk: connect attempt %d failed: %s\n", attempt, err.c_str());
      if (err == "THE API KEY WAS REFUSED" || err == "UNKNOWN MODEL") break;  // retrying won't help
      vTaskDelay(pdMS_TO_TICKS(800));
    }
    connAuth = "";  // it held the key
    if (err.length()) return post(EV_FAIL, err.c_str());
    open = true;
    micTail = micHead;
    head = tail = 0;
    ws.sendText(startJson.c_str(), startJson.length());
    if (!live) post(EV_READY);  // GPT-Live answers with session.started first
  }

  void sendMic() {
    const char* h = live ? "{\"type\":\"session.input_audio.append\",\"audio\":\"" : "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    const size_t hl = strlen(h);
    static int16_t chunk[CHUNK];
    for (int i = 0; i < CHUNK; i++) chunk[i] = mic[micTail++ % MIC_RING];
    memcpy(out, h, hl);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char*)out + hl, CHUNK * 3, &olen, (const unsigned char*)chunk, CHUNK * 2);
    memcpy(out + hl + olen, "\"}", 3);
    ws.sendText(out, hl + olen + 2);
  }

  void sendSilence() {
    static const char H[] = "{\"type\":\"session.input_audio.append\",\"audio\":\"";
    const size_t hl = sizeof H - 1, al = CHUNK * 2 / 3 * 4;  // every 3 zero bytes are "AAAA"
    memcpy(out, H, hl);
    memset(out + hl, 'A', al);
    memcpy(out + hl + al, "\"}", 3);
    ws.sendText(out, hl + al + 2);
  }

  void onMessage(char* m) {
    const char* t = strstr(m, "\"type\":\"");
    if (!t) return;
    t += 8;
    if (!strncmp(t, "response.output_audio.delta", 27) || !strncmp(t, "response.audio.delta", 20) ||
        !strncmp(t, "session.output_audio.delta", 26)) {
      return onAudio(m);
    }
    if (strncmp(t, "session.output_transcript", 25) && strncmp(t, "session.usage", 13) &&
        strncmp(t, "response.output_audio_transcript", 32))
      Serial.printf("talk: event %.80s\n", t);
    if (!strncmp(t, "session.started", 15)) post(EV_READY);
    else if (!strncmp(t, "response.done", 13)) post(EV_DONE);
    else if (!strncmp(t, "session.closed", 14)) post(EV_CLOSED);
    else if (!strncmp(t, "input_audio_buffer.speech_started", 33) || !strncmp(t, "session.input_transcript.delta", 30)) {
      if (millis() - heardAt > 300) heardAt = millis(), post(EV_HEARD);
    } else if (!strncmp(t, "error", 5) && t[5] == '"') {
      Serial.printf("openai error: %s\n", m);
      if (strstr(m, "buffer too small") || strstr(m, "Cancellation failed") || strstr(m, "no active response")) return;
      char buf[96] = "OPENAI REPORTED AN ERROR";
      if (strstr(m, "\"invalid_model\"")) {
        snprintf(buf, sizeof buf, "%s IS NOT A %s MODEL", modelName.c_str(), live ? "GPT-LIVE" : "REALTIME");
      } else if (const char* msg = strstr(m, "\"message\":\"")) {
        // the message as far as its closing quote; \" inside it is a quote, other escapes are skipped
        msg += 11;
        size_t n = 0;
        for (; *msg && n < sizeof buf - 1; msg++) {
          if (*msg == '\\') {
            if (!*++msg) break;
            if (*msg == '"') buf[n++] = '"';
            continue;
          }
          if (*msg == '"') break;
          buf[n++] = toupper(*msg);
        }
        buf[n] = 0;
      }
      post(EV_FAIL, buf);
    }
  }

  void onAudio(char* m) {
    if (discarding) {  // the rest of a stopped reply, until it goes quiet
      if (millis() - lastDeltaAt < 800) {
        lastDeltaAt = millis();
        return;
      }
      discarding = false;
    }
    char* d = strstr(m, "\"delta\":\"");
    if (!d) return;
    d += 9;
    char* end = strchr(d, '"');
    if (!end) return;
    size_t olen = 0;  // decode in place: the PCM is shorter than its base64
    if (mbedtls_base64_decode((unsigned char*)d, end - d, &olen, (const unsigned char*)d, end - d) != 0) return;
    lastDeltaAt = millis();
    const uint8_t* pcm = (const uint8_t*)d;
    for (size_t i = 0; i + 1 < olen && head - tail < RING; i += 2) {
      int16_t v;
      memcpy(&v, pcm + i, 2);  // may be unaligned
      uint8_t& e = env[(head / ENV_BLOCK) % ENV_COUNT];
      if (head % ENV_BLOCK == 0) e = 0;
      e = max<uint8_t>(e, min(255, abs(v) >> 6));
      ring[head % RING] = v;
      head = head + 1;
    }
  }
};

// ---------- the orb ----------
// A pixel blob in Tock's mustard.

enum OrbMood { ORB_CONNECTING, ORB_PAUSED, ORB_LISTENING, ORB_THINKING, ORB_SPEAKING, ORB_ERROR };

class Orb {
 public:
  static constexpr int CELL = 4, X = 160, Y = 104;

  void draw(Gfx& g, OrbMood mood, float level, uint32_t now) {
    const float t = animSecs(now);
    float target = 30, amp = 1.5f, speed = 1.2f;
    uint16_t core = pal::body, mid = pal::amber, edge = pal::dim;
    switch (mood) {
      case ORB_CONNECTING: target = 18 + 3 * sinf(t * 3); break;
      case ORB_PAUSED: target = 30 + 1.5f * sinf(t * 1.5f); core = pal::amber, mid = pal::dim, edge = pal::umber; break;
      case ORB_LISTENING: target = 34 + level * 26, amp = 1.5f + level * 7, speed = 3; break;
      case ORB_THINKING: target = 30 + 2 * sinf(t * 2), amp = 1; break;
      case ORB_SPEAKING: target = 36 + level * 20, amp = 1 + level * 4, speed = 2.5f; break;
      case ORB_ERROR: target = 24, amp = 0, core = pal::grey, mid = pal::grey, edge = pal::faint; break;
    }
    // eases 30% of the way per 33 ms, whatever the frame rate
    const float k = 1 - powf(0.7f, min<uint32_t>(200, now - lastDraw) / 33.0f);
    lastDraw = now;
    r += (target - r) * k;

    if (mood == ORB_SPEAKING)  // ripples drifting outward and fading
      for (int k = 0; k < 2; k++) {
        const float p = fmodf(t * 0.9f + k * 0.5f, 1);
        ring(g, r + 6 + p * 20, p < 0.5f ? edge : pal::faint);
      }

    const int reach = (int)ceilf((r + amp + 2) / CELL) * CELL;
    for (int y = -reach; y < reach; y += CELL)
      for (int x = -reach; x < reach; x += CELL) {
        const float cx = x + CELL / 2.0f, cy = y + CELL / 2.0f;
        const float d = sqrtf(cx * cx + cy * cy);
        if (d > r + amp + 1) continue;
        const float a = atan2f(cy, cx);
        const float rim = r + amp * (0.6f * sinf(3 * a + t * speed * 2.1f) + 0.4f * sinf(5 * a - t * speed * 1.3f));
        if (d > rim) continue;
        const float k = d / rim;
        g.fillRect(X + x, Y + y, CELL, CELL, k < 0.5f ? core : k < 0.82f ? mid : edge);
      }

    if (mood == ORB_THINKING)  // three dots circling
      for (int k = 0; k < 3; k++) {
        const float a = t * 3.2f + k * 2.0944f;
        const int x = (int)roundf((X + cosf(a) * (r + 14)) / 2) * 2, y = (int)roundf((Y + sinf(a) * (r + 14)) / 2) * 2;
        g.fillRect(x - 3, y - 3, 6, 6, k == 0 ? pal::body : pal::amber);
      }
  }

 private:
  float r = 20;
  uint32_t lastDraw = 0;

  void ring(Gfx& g, float rr, uint16_t color) {
    const int reach = (int)ceilf((rr + CELL) / CELL) * CELL;
    for (int y = -reach; y < reach; y += CELL)
      for (int x = -reach; x < reach; x += CELL) {
        const float cx = x + CELL / 2.0f, cy = y + CELL / 2.0f;
        if (fabsf(sqrtf(cx * cx + cy * cy) - rr) < CELL / 2.0f) g.fillRect(X + x, Y + y, CELL, CELL, color);
      }
  }
};

// ---------- the app ----------

class TalkApp : public App {
 public:
  const char* name() override { return "TALK"; }

  void subtitle(char* out, size_t n) override {
    if (!net::aiKey().length()) snprintf(out, n, "NO API KEY YET");
    else if (!net::online()) snprintf(out, n, "NEEDS WI-FI");
    else snprintf(out, n, "TALK WITH TOCK");
  }

  // A mustard speech bubble on a dark tile; the dots blink when selected.
  void drawIcon(Gfx& g, int x, int y, uint32_t now, bool live) override {
    static const char* const BUBBLE[] = {
      ".###########.", "#############", "#############", "###.##.##.###", "#############",
      "#############", ".###########.", "..###........", "..##.........", "..#..........",
    };
    constexpr int PX = 3;
    g.fillRect(x, y, ICON_SIZE, ICON_SIZE, rgb565(0x26231f));
    const int bx = x + (ICON_SIZE - 13 * PX) / 2, by = y + (ICON_SIZE - 10 * PX) / 2;
    g.drawGrid(BUBBLE, 10, bx, by, PX, rgb565(0xe7ae45));
    if (live && (now / 400) % 2) g.fillRect(bx + 3 * PX, by + 3 * PX, 7 * PX, PX, rgb565(0xe7ae45));
  }

  bool quiet() override { return true; }

  bool connecting() const { return phase == P_CONNECTING; }

  void enter(uint32_t now) override {
    link.begin();
    M5.Speaker.end();  // Talk's own output (voice.h) takes the speaker
    voice::begin();
    aHeld = false;
    decide(now);
  }

  void leave(uint32_t now) override {
    stopMic();
    link.silence = false;
    link.close();
    link.end();
    voice::end();
    M5.Speaker.begin();
    phase = P_OFF;
  }

  void button(Btn b, BtnEv ev, uint32_t now) override {
    if (b == BTN_A && ev == EV_PRESS) {
      aHeld = true;
      switch (phase) {
        case P_READY:
          if (asleep) decide(now);  // hung up while idle: call back, record once it answers
          else startRecording(now);
          break;
        case P_THINKING:
        case P_SPEAKING:
          stopReply();  // talking over Tock cuts it off
          startRecording(now);
          break;
        case P_NEED_WIFI:
          if (net::wstate == net::W_FAILED) net::wifiConnect(now);  // try again now, not in a minute
          decide(now);
          break;
        case P_NEED_KEY:
        case P_ERROR: decide(now); break;
        default: break;
      }
    } else if (b == BTN_A && ev == EV_RELEASE) {
      aHeld = false;
      if (phase == P_RECORDING) send(now);
    } else if (b == BTN_B && ev == EV_CLICK) {
      if (phase == P_THINKING || phase == P_SPEAKING) {
        stopReply();
        toReady(now);
      } else if (phase == P_NEED_KEY || phase == P_NEED_WIFI || phase == P_ERROR) {
        decide(now);
      }
    }
  }

  void update(uint32_t now, float dt) override {
    TalkEvent e;
    while (link.poll(e)) {
      switch (e.kind) {
        case EV_READY:
          if (phase == P_CONNECTING) {
            toReady(now);
            if (aHeld) startRecording(now);  // still holding A from the call-back
          }
          break;
        case EV_FAIL: fail(e.text); break;
        case EV_CLOSED:
          if (phase != P_OFF && phase != P_ERROR && !asleep) fail("THE CONNECTION DROPPED");
          break;
        case EV_DONE: responseDone = true; break;
        default: break;
      }
    }
    if (phase == P_NEED_WIFI && net::online()) decide(now);  // just after boot, Wi-Fi takes a few seconds
    // more of the reply after it looked finished (GPT-Live pauses while it looks something up): play it
    if (phase == P_READY && link.available() > 0 && !link.discarding) beginSpeaking(now);
    if (phase == P_CONNECTING && (int32_t)(now - phaseSince) > 20000) return fail("NO ANSWER FROM OPENAI");
    if (phase == P_RECORDING) record(now);
    if (phase == P_THINKING) think(now);
    if (phase == P_SPEAKING) speak(now);
    if (phase == P_READY && !asleep && (int32_t)(now - phaseSince) > 60000) {
      link.close();  // GPT-Live bills by the minute
      asleep = true;
    }
  }

  void draw(Gfx& g, uint32_t now) override {
    g.fillScreen(pal::bg);
    g.drawText("TALK", 16, 14, 2, pal::grey);
    String model = net::aiModel();
    model.toUpperCase();
    g.drawText(model.substring(0, 18).c_str(), SCREEN_W - 16, 14, 2, pal::grey, RIGHT);

    static const OrbMood MOOD[] = {ORB_ERROR, ORB_ERROR, ORB_ERROR, ORB_CONNECTING, ORB_PAUSED,
                                   ORB_LISTENING, ORB_THINKING, ORB_SPEAKING, ORB_ERROR};
    const bool joining = phase == P_NEED_WIFI && net::wstate == net::W_CONNECTING;
    orb.draw(g, joining ? ORB_CONNECTING : MOOD[phase], level, now);

    const char* label = "";
    const char* hint = "";
    switch (phase) {
      case P_NEED_KEY: label = "NO API KEY YET"; hint = "SEND ONE FROM THE MAC APP"; break;
      case P_NEED_WIFI:
        switch (net::wstate) {
          case net::W_CONNECTING: label = "JOINING WI-FI"; break;
          case net::W_OFF: label = "WI-FI IS OFF"; hint = "TURN IT ON IN SETTINGS"; break;
          case net::W_NO_SETUP: label = "NO WI-FI SET UP"; hint = "SEND ONE FROM THE MAC APP"; break;
          default: label = "WI-FI DID NOT CONNECT"; hint = "PRESS A TO TRY AGAIN"; break;
        }
        break;
      case P_CONNECTING: label = "CONNECTING"; break;
      case P_READY: label = "READY"; hint = "HOLD A TO TALK"; break;
      case P_RECORDING: label = "RECORDING"; hint = "LET GO TO SEND"; break;
      case P_THINKING: label = "THINKING"; hint = "B TO CANCEL"; break;
      case P_SPEAKING: label = "SPEAKING"; hint = "B TO STOP   HOLD A TO REPLY"; break;
      case P_ERROR: hint = "PRESS A TO TRY AGAIN"; break;
      default: break;
    }
    if (phase == P_ERROR) {
      drawWrapped(g, error, 178);
    } else {
      if (phase == P_RECORDING && (now / 400) % 2 == 0)  // a transmit light
        g.fillRect(SCREEN_W / 2 - g.textWidth(label, 2) / 2 - 14, 191, 8, 8, pal::red);
      g.drawText(label, SCREEN_W / 2, 190, 2, phase == P_READY ? pal::grey : pal::ink, CENTER);
    }
    g.drawText(hint, SCREEN_W / 2, 214, 2, pal::grey, CENTER);
  }

  void drawLeds(Leds& leds, uint32_t now) override {
    if (phase == P_RECORDING || phase == P_SPEAKING) {
      const float lit = level * LEDS_PER_BAR * 1.3f;
      for (int i = 0; i < LEDS_PER_BAR; i++) leds.setBoth(i, 0xe7ae45, constrain(lit - i, 0.0f, 1.0f));
    } else if (phase == P_THINKING || phase == P_CONNECTING) {
      leds.setBoth((now / 150) % LEDS_PER_BAR, 0xe7ae45, 0.6f);
    }
  }

 private:
  enum Phase { P_OFF, P_NEED_KEY, P_NEED_WIFI, P_CONNECTING, P_READY, P_RECORDING, P_THINKING, P_SPEAKING, P_ERROR };

  static constexpr int RATE = TalkLink::RATE;
  static constexpr size_t PREROLL = RATE * 6 / 10;  // start speaking once 0.6 s is buffered
  static constexpr size_t AHEAD = RATE;             // keep up to 1 s queued with the speaker
  static constexpr uint32_t MIN_MESSAGE_MS = 300;   // shorter than this was a tap, not a message

  TalkLink link;
  Orb orb;
  Phase phase = P_OFF;
  bool gptLive = false, asleep = false, aHeld = false;
  uint32_t phaseSince = 0;
  bool responseDone = false, started = false;
  float level = 0;
  uint32_t levelAt = 0;
  char error[96] = "";

  size_t playFrom = 0, playedAt = 0;  // where in the reply the speaker started, to follow it
  uint32_t speakStartMs = 0;
  float hpX = 0, hpY = 0;

  void setPhase(Phase p, uint32_t now) {
    phase = p;
    phaseSince = now;
  }

  static constexpr const char* PERSONA =
    "You are Tock, a tiny mustard-colored kitchen timer who lives on a desk and helps your owner focus. "
    "Speak warmly and briefly: one to three short sentences.";

  void decide(uint32_t now) {
    link.close();
    link.silence = false;
    asleep = false;
    stopMic();
    if (!net::aiKey().length()) return setPhase(P_NEED_KEY, now);
    if (!net::online()) return setPhase(P_NEED_WIFI, now);
    const String model = net::aiModel();
    gptLive = model.startsWith("gpt-live");
    String start;
    if (gptLive) {
      // the thinking goes to a Responses model, which may search the web
      start = String("{\"type\":\"session.start\",\"session\":{\"model\":\"") + model + "\",\"instructions\":\"" + PERSONA +
              "\",\"audio\":{\"output\":{\"voice\":\"marin\"}},\"delegation\":{\"type\":\"responses\",\"responses\":{"
              "\"model\":\"" + LIVE_BACKEND + "\",\"tools\":[{\"type\":\"web_search\"}],\"tool_choice\":\"auto\"}}}}";
    } else {
      start = String("{\"type\":\"session.update\",\"session\":{\"type\":\"realtime\",\"instructions\":\"") + PERSONA +
              "\",\"audio\":{\"input\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},\"turn_detection\":null},"
              "\"output\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},\"voice\":\"marin\"}}}}";
    }
    link.connect(gptLive, model, gptLive ? String("/v1/live/sessions") : "/v1/realtime?model=" + model,
                 "Authorization: Bearer " + net::aiKey(), start);
    setPhase(P_CONNECTING, now);
  }

  void fail(const char* why) {
    stopMic();
    link.silence = false;
    voice::cut();
    link.close();
    snprintf(error, sizeof error, "%s", why);
    phase = P_ERROR;
  }

  void toReady(uint32_t now) {
    link.silence = false;
    if (gptLive) link.send("{\"type\":\"session.input_audio.mute\"}");
    level = 0;
    setPhase(P_READY, now);
  }

  // ---------- recording (A held) ----------

  void startRecording(uint32_t now) {
    link.silence = false;
    link.dropPlayback();  // anything older than this message is stale
    voice::end();  // the mic needs the I2S port the speaker uses
    if (!mic::begin(RATE)) return fail("THE MICROPHONE DID NOT START");
    link.flushMic();
    link.send(gptLive ? "{\"type\":\"session.input_audio.unmute\"}" : "{\"type\":\"input_audio_buffer.clear\"}");
    level = 0;
    setPhase(P_RECORDING, now);
  }

  void stopMic() {
    mic::end();
    voice::begin();
  }

  void record(uint32_t now) {
    int16_t buf[1024];
    int32_t peak = 0;
    for (;;) {
      const size_t n = mic::read(buf, 1024);
      if (!n) break;
      for (size_t i = 0; i < n; i++) peak = max<int32_t>(peak, abs(buf[i]));
      link.pushMic(buf, n);
    }
    const float k = 1 - powf(0.6f, min<uint32_t>(200, now - levelAt) / 33.0f);  // 40% per 33 ms
    levelAt = now;
    level += (min(1.0f, peak / 14000.0f) - level) * k;
  }

  // A let go: send the message (or drop it if it was only a tap).
  void send(uint32_t now) {
    record(now);  // the last few words
    stopMic();
    if ((int32_t)(now - phaseSince) < (int32_t)MIN_MESSAGE_MS) {
      link.flushMic();
      if (!gptLive) link.send("{\"type\":\"input_audio_buffer.clear\"}");
      return toReady(now);
    }
    if (gptLive) {
      link.send("{\"type\":\"session.input_audio.mute\"}");  // muting ends the turn
    } else {
      link.send("{\"type\":\"input_audio_buffer.commit\"}");
      link.send("{\"type\":\"response.create\"}");
    }
    responseDone = started = false;
    level = 0;
    setPhase(P_THINKING, now);
  }

  // ---------- thinking and speaking ----------

  void think(uint32_t now) {
    if (link.available() > 0) return beginSpeaking(now);
    // GPT-Live: if muting didn't prompt an answer, feed it silence so it hears the pause
    if (gptLive && !link.silence && (int32_t)(now - phaseSince) > 3000) {
      Serial.println("talk: no reply to mute, streaming silence");
      link.send("{\"type\":\"session.input_audio.unmute\"}");
      link.silence = true;
    }
    if (responseDone || (int32_t)(now - phaseSince) > 25000) toReady(now);  // no answer came
  }

  void beginSpeaking(uint32_t now) {
    started = false;
    level = 0;
    setPhase(P_SPEAKING, now);
  }

  // For a tiny 8-bit speaker: cut the bass it can't play, lift the voice, round off peaks.
  int16_t shape(int16_t x) {
    constexpr float A = 0.955f;  // one-pole high-pass, about 180 Hz at 24 kHz
    hpY = A * (hpY + x - hpX);
    hpX = x;
    float v = hpY * 2.2f;
    const float knee = 20000;
    if (v > knee) v = knee + (v - knee) * 0.25f;
    else if (v < -knee) v = -knee + (v + knee) * 0.25f;
    return (int16_t)constrain(v, -32767.0f, 32767.0f);
  }

  void speak(uint32_t now) {
    const bool done = gptLive ? (int32_t)(millis() - link.lastDeltaAt) > 2500 : responseDone;
    if (!started && (link.available() >= PREROLL || (done && link.available() > 0))) {
      started = true;
      playFrom = link.tail;
      playedAt = voice::played;
      speakStartMs = now;
      hpX = hpY = 0;
    }
    if (started) {
      // gaps in the stream just play as silence; the speaker never stops mid-reply
      int16_t buf[480];
      while (voice::queued() < AHEAD && link.available() > 0) {
        const size_t n = min<size_t>({link.available(), voice::space(), 480});
        if (!n) break;
        size_t t = link.tail;
        for (size_t i = 0; i < n; i++) buf[i] = shape(link.ring[t++ % TalkLink::RING]);
        voice::push(buf, n);
        link.tail = t;
      }
      // the orb follows what is coming out of the speaker now, not what was just queued
      const size_t audible = playFrom + (voice::played - playedAt);
      level = audible < link.tail ? min(1.0f, link.env[(audible / TalkLink::ENV_BLOCK) % TalkLink::ENV_COUNT] / 160.0f) : 0;
    }
    if (done && link.available() == 0 && voice::queued() == 0) {  // your turn
      if (started) {
        const uint32_t ms = now - speakStartMs;
        Serial.printf("talk: played %u samples in %u ms (%u Hz)\n", (unsigned)(voice::played - playedAt), (unsigned)ms,
                      ms ? (unsigned)((uint64_t)(voice::played - playedAt) * 1000 / ms) : 0);
      }
      toReady(now);
    }
  }

  void stopReply() {
    voice::cut();
    link.silence = false;
    link.discarding = true;
    link.dropPlayback();
    if (!gptLive) link.send("{\"type\":\"response.cancel\"}");
  }

  // Up to two centered lines at scale 2; anything longer ends in "...".
  static void drawWrapped(Gfx& g, const char* text, int y) {
    constexpr int MAX_W = SCREEN_W - 24;
    char lines[2][64] = {"", ""};
    int line = 0;
    const char* p = text;
    while (*p && line < 2) {
      const char* end = strchr(p, ' ');
      const size_t wl = end ? end - p : strlen(p);
      char trial[128];
      snprintf(trial, sizeof trial, "%s%s%.*s", lines[line], *lines[line] ? " " : "", (int)wl, p);
      if (g.textWidth(trial, 2) <= MAX_W && strlen(trial) < sizeof lines[0]) {
        strcpy(lines[line], trial);
        p += wl;
        while (*p == ' ') p++;
      } else if (!*lines[line]) {  // one word too wide on its own: cut it
        snprintf(lines[line], sizeof lines[0], "%.*s", 30, p);
        p += min<size_t>(wl, 30);
      } else {
        line++;
      }
    }
    if (*p && line >= 1) strncat(lines[1], "...", sizeof lines[1] - strlen(lines[1]) - 1);
    const bool two = *lines[1];
    g.drawText(lines[0], SCREEN_W / 2, two ? y : y + 7, 2, pal::ink, CENTER);
    if (two) g.drawText(lines[1], SCREEN_W / 2, y + 14, 2, pal::ink, CENTER);
  }
};
