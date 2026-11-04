// crowpanel-display firmware skeleton.
//
// "Dumb framebuffer" architecture: the host renders 1bpp frames and sends
// them over UART (PROTOCOL.md at the repo root), the firmware draws them
// and reports button events back. Everything personal stays on the host;
// the only thing living here is the fail-safe: no heartbeat - the panel
// draws a neutral screen by itself.
//
// Done: protocol parser (binary frames + debug lines), buttons with long
// press, quadrature encoder, heartbeat, refresh policy, dual SSD1683
// driver (Ssd1683Epd).
//
// The driver is blocking for the length of a wave (1.7 s work / 2.3 s
// full): buttons are not polled while the glass updates, events from
// that window are lost. Known v1 limitation.

#include <Arduino.h>

#include "protocol.h"
#include "ssd1683_epd.h"

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 460800
#endif

// ---------- pins (Elecrow wiki, confirmed on a live panel) ----------
// EPD pins live in ssd1683_epd.cpp; here are the inputs only.

static const int PIN_MENU = 2;
static const int PIN_EXIT = 1;
static const int PIN_ENC_A = 6;  // "Up" in the wiki
static const int PIN_ENC_B = 4;  // "Down" in the wiki
static const int PIN_ENC_CONF = 5;

// ---------- forward declarations ----------

static void send_pong();
static void set_mode(uint8_t m);
static void handle_frame(uint8_t cmd, const uint8_t* p, uint16_t len);
static uint32_t crc32_update(const uint8_t* data, size_t len, uint32_t c);

// ---------- CRC32 (IEEE, same as zlib.crc32 on the host) ----------

static uint32_t crc32_table[256];

static void crc32_init() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc32_table[i] = c;
  }
}

static uint32_t crc32_update(const uint8_t* data, size_t len, uint32_t c) {
  for (size_t i = 0; i < len; i++) c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c;
}

static uint32_t crc32_calc(const uint8_t* data, size_t len) {
  return crc32_update(data, len, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
}

// ---------- protocol: sending upstream ----------

static uint8_t frame_buf[MAX_PAYLOAD];  // payload receive buffer (64 KB)

static void send_up(uint8_t cmd, const uint8_t* payload, uint16_t len) {
  uint8_t head[5] = {FRAME_MAGIC[0], FRAME_MAGIC[1], cmd, (uint8_t)(len & 0xFF),
                     (uint8_t)(len >> 8)};
  Serial0.write(head, 5);
  if (len) Serial0.write(payload, len);
  uint8_t body_for_crc[3] = {cmd, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  uint32_t crc = crc32_update(body_for_crc, 3, 0xFFFFFFFFu);
  if (len) crc = crc32_update(payload, len, crc);
  crc ^= 0xFFFFFFFFu;
  uint8_t crc_le[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16),
                       (uint8_t)(crc >> 24)};
  Serial0.write(crc_le, 4);
}

static void send_key(uint8_t key, uint8_t event, int16_t value) {
  uint8_t p[4] = {key, event, (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF)};
  send_up(CMD_KEY_EVENT, p, 4);
}

// ---------- display driver ----------

static Ssd1683Epd epd;  // orientation defaults; flip flags after the marker check

// ---------- mode and refresh policy ----------
//
// Workhorse updates (0xC7, ~1.7 s, no flashing, slightly grayish black)
// carry regular frames; a full flashing refresh (0xF7) runs on mode
// transitions, every FULL_REFRESH_EVERY frames and by request - it
// cleans up ghosting but flashes the panel.

static uint8_t mode = MODE_SPLASH;
static uint32_t frames_since_full = 0;
static const uint32_t FULL_REFRESH_EVERY = 50;  // full refresh once per N frames
static const uint32_t RECT_FULL_THRESHOLD = (uint32_t)EPD_WIDTH * EPD_HEIGHT / 2;
static bool force_full_next = false;

static bool take_full_refresh() {
  if (force_full_next || frames_since_full >= FULL_REFRESH_EVERY) {
    force_full_next = false;
    frames_since_full = 0;
    return true;
  }
  frames_since_full++;
  return false;
}

static void set_mode(uint8_t m) {
  if (m > MODE_NEUTRAL) {
    Serial0.printf("[rx] SET_MODE out of range: %u\n", (unsigned)m);
    return;
  }
  mode = m;
  if (m == MODE_NEUTRAL) {
    force_full_next = true;  // entering neutral also forces a full refresh
  }
}

// ---------- receive: text debug mode ----------

static char text_line[96];
static uint8_t text_len = 0;

static void handle_text_line(char* line) {
  if (!strcmp(line, "ping")) {
    send_pong();
  } else if (!strncmp(line, "mode ", 5)) {
    set_mode(atoi(line + 5));
  } else if (!strcmp(line, "refresh")) {
    force_full_next = true;
    Serial0.printf("[epd] full refresh on next frame\n");
  }
  // unrecognized lines are ignored (may be resynchronization garbage)
}

static void feed_text(uint8_t b) {
  if (b == '\n' || b == '\r') {
    if (text_len) {
      text_line[text_len] = 0;
      handle_text_line(text_line);
      text_len = 0;
    }
    return;
  }
  // only printable bytes enter text mode (the magic is non-ASCII)
  if (b >= 0x20 && b < 0x7F && text_len < sizeof(text_line) - 1) {
    text_line[text_len++] = (char)b;
  } else {
    text_len = 0;  // garbage mid-line - start over
  }
}

// ---------- receive: binary frames ----------

struct RxState {
  enum Phase { SYNC1, SYNC2, CMD, LEN1, LEN2, PAYLOAD, CRC1, CRC2, CRC3, CRC4 };
  Phase phase = SYNC1;
  uint8_t cmd = 0;
  uint16_t len = 0;
  uint16_t got = 0;
  uint8_t hdr[3];  // cmd + len(2) - start of the frame body for CRC
  uint32_t crc_got = 0;
  uint32_t last_byte_ms = 0;  // receive timeout: a break mid-frame
};

static const uint32_t RX_FRAME_TIMEOUT_MS = 500;

static RxState rx;
static uint32_t last_host_frame_ms = 0;  // heartbeat fail-safe

static void feed_byte(uint8_t b) {
  rx.last_byte_ms = millis();
  switch (rx.phase) {
    case RxState::SYNC1:
      if (b == FRAME_MAGIC[0]) rx.phase = RxState::SYNC2;
      else feed_text(b);
      return;
    case RxState::SYNC2:
      if (b == FRAME_MAGIC[1]) rx.phase = RxState::CMD;
      else { rx.phase = RxState::SYNC1; feed_text(b); }
      return;
    case RxState::CMD:
      rx.cmd = b; rx.hdr[0] = b; rx.phase = RxState::LEN1; return;
    case RxState::LEN1:
      rx.len = b; rx.hdr[1] = b; rx.phase = RxState::LEN2; return;
    case RxState::LEN2: {
      rx.len |= ((uint16_t)b) << 8; rx.hdr[2] = b;
      if (rx.len > MAX_PAYLOAD) { rx.phase = RxState::SYNC1; return; }
      rx.got = 0;
      // with len == 0 there is no payload phase at all - straight to CRC
      rx.phase = rx.len ? RxState::PAYLOAD : RxState::CRC1;
      return;
    }
    case RxState::PAYLOAD:
      if (rx.got < rx.len) frame_buf[rx.got++] = b;
      if (rx.got == rx.len) rx.phase = RxState::CRC1;
      return;
    case RxState::CRC1: rx.crc_got = b; rx.phase = RxState::CRC2; return;
    case RxState::CRC2: rx.crc_got |= ((uint32_t)b) << 8; rx.phase = RxState::CRC3; return;
    case RxState::CRC3: rx.crc_got |= ((uint32_t)b) << 16; rx.phase = RxState::CRC4; return;
    case RxState::CRC4: {
      rx.crc_got |= ((uint32_t)b) << 24;
      // CRC is computed in one continuous pass: hdr + payload, a single
      // final xor; crc32_calc does not fit here (it xors at the end)
      uint32_t crc = crc32_update(rx.hdr, 3, 0xFFFFFFFFu);
      if (rx.got) crc = crc32_update(frame_buf, rx.got, crc);
      crc ^= 0xFFFFFFFFu;
      if (crc == rx.crc_got) {
        handle_frame(rx.cmd, frame_buf, rx.len);
        last_host_frame_ms = millis();
      } else {
        Serial0.printf("[rx] bad CRC, frame dropped\n");
      }
      rx.phase = RxState::SYNC1;
      return;
    }
    default:
      rx.phase = RxState::SYNC1;
  }
}

// ---------- host command handlers ----------

static void send_pong() {
  uint32_t up = millis();
  uint8_t p[5] = {(uint8_t)up, (uint8_t)(up >> 8), (uint8_t)(up >> 16),
                  (uint8_t)(up >> 24), mode};
  send_up(CMD_PONG, p, 5);
}

static void handle_frame(uint8_t cmd, const uint8_t* p, uint16_t len) {
  switch (cmd) {
    case CMD_PING:
      send_pong();
      break;
    case CMD_FRAME_FULL:
      if (len == FULL_FRAME_BYTES) {
        epd.drawFull(p, take_full_refresh());
      } else {
        Serial0.printf("[rx] FRAME_FULL wrong size: %u\n", (unsigned)len);
      }
      break;
    case CMD_FRAME_RECT:
      if (len >= 8) {
        uint16_t x = p[0] | (p[1] << 8), y = p[2] | (p[3] << 8);
        uint16_t w = p[4] | (p[5] << 8), h = p[6] | (p[7] << 8);
        uint32_t expect = 8 + (uint32_t)((w + 7) / 8) * h;
        if (w == 0 || h == 0 || x + w > EPD_WIDTH || y + h > EPD_HEIGHT) {
          Serial0.printf("[rx] FRAME_RECT out of bounds: %u,%u %ux%u\n",
                         (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
        } else if (len != expect) {
          Serial0.printf("[rx] FRAME_RECT size %u != %u (stride*h)\n",
                         (unsigned)len, (unsigned)expect);
        } else {
          // a rect over half the screen is worth a clean full refresh
          bool full = (uint32_t)w * h >= RECT_FULL_THRESHOLD || take_full_refresh();
          epd.drawRect(x, y, w, h, p + 8, full);
        }
      }
      break;
    case CMD_CLEAR:
      if (len >= 1) epd.clear(p[0]);
      break;
    case CMD_SET_MODE:
      if (len >= 1) set_mode(p[0]);
      break;
    case CMD_FULL_REFRESH:
      force_full_next = true;
      break;
    default:
      Serial0.printf("[rx] unknown command 0x%02x\n", cmd);
  }
}

// ---------- neutral screen (privacy fail-safe) ----------

// Same intent as neutral_frame() on the host: a clock without personal
// data. For now a plain white screen; text arrives together with the
// driver (EPD_ShowString from the Elecrow library or a custom bitmap
// font).
static void draw_neutral_builtin() {
  Serial0.printf("[fail-safe] no heartbeat - drawing neutral screen\n");
  memset(frame_buf, 0, FULL_FRAME_BYTES);  // 0 = white (protocol: 1 = black)
  epd.drawFull(frame_buf, true);           // neutral always drives full
  mode = MODE_NEUTRAL;
}

// ---------- buttons and encoder ----------

struct Btn {
  uint8_t pin;
  Key code;
  bool down = false;
  uint32_t down_ms = 0;
  uint32_t up_ms = 0;
  bool long_sent = false;
};

static Btn buttons[] = {
    {PIN_MENU, KEY_MENU}, {PIN_EXIT, KEY_EXIT}, {PIN_ENC_CONF, KEY_ENC_CONF},
};

static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONGPRESS_MS = 700;

// quadrature: a transition table over the 4-bit window of the two phases
static const int8_t ENC_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
static uint8_t enc_state = 0;

static void poll_encoder() {
  static uint32_t last_poll = 0;
  uint32_t now = millis();
  if (now - last_poll < 2) return;
  last_poll = now;
  uint8_t ab = (digitalRead(PIN_ENC_A) ? 2 : 0) | (digitalRead(PIN_ENC_B) ? 1 : 0);
  enc_state = ((enc_state << 2) | ab) & 0x0F;
  int8_t d = ENC_TABLE[enc_state];
  if (d) send_key(d > 0 ? KEY_ENC_UP : KEY_ENC_DOWN, EV_SHORT, d);
}

static void poll_buttons() {
  uint32_t now = millis();
  for (auto& b : buttons) {
    bool pressed = digitalRead(b.pin) == LOW;  // active LOW (pullup)
    if (pressed && !b.down && now - b.up_ms > DEBOUNCE_MS) {
      b.down = true;
      b.down_ms = now;
      b.long_sent = false;
      send_key(b.code, EV_DOWN, 0);
    } else if (pressed && b.down && !b.long_sent && now - b.down_ms >= LONGPRESS_MS) {
      b.long_sent = true;
      send_key(b.code, EV_LONG, (int16_t)(now - b.down_ms));
      if (b.code == KEY_MENU) force_full_next = true;  // long MENU - full refresh
    } else if (!pressed && b.down) {
      b.down = false;
      b.up_ms = now;
      if (!b.long_sent) send_key(b.code, EV_SHORT, 0);
      send_key(b.code, EV_UP, 0);
    }
  }
}

// ---------- button diagnostics: scanning free GPIOs ----------
// The button pin map IO1/2/4/5/6 is confirmed by live events (2026-11-03).
// The scan stays as transition diagnostics. Do NOT add IO33-37 to
// SCAN_PINS: on the ESP32-S3R8 (octal PSRAM, qio_opi) those are MSPI bus
// lines - pinMode on them steals the lines from the controller, and the
// first received byte (the UART RX ring lives in PSRAM) hangs the bus:
// TG1WDT_SYS_RST after ~1 s.

static const uint8_t SCAN_PINS[] = {
    0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 13, 14, 15, 16, 17, 18, 19, 21,
    41, 42,
};

static void poll_key_debug() {
  static uint32_t last_poll = 0;
  if (millis() - last_poll < 10) return;  // 100 Hz poll: debounce visible, no noise
  last_poll = millis();
  static uint32_t last = 0;
  static bool first = true;
  uint32_t cur = 0;
  for (uint8_t i = 0; i < sizeof(SCAN_PINS); i++) {
    pinMode(SCAN_PINS[i], INPUT_PULLUP);
    cur |= (digitalRead(SCAN_PINS[i]) ? 1u : 0u) << i;
  }
  if (first) {
    first = false;
    Serial0.print("[scan] boot levels:");
    for (uint8_t i = 0; i < sizeof(SCAN_PINS); i++)
      Serial0.printf(" IO%d=%d", SCAN_PINS[i], (cur >> i) & 1);
    Serial0.printf("\n");
  } else if (cur != last) {
    for (uint8_t i = 0; i < sizeof(SCAN_PINS); i++) {
      if (((cur ^ last) >> i) & 1)
        Serial0.printf("[scan] IO%d: %d -> %d\n", SCAN_PINS[i],
                       (last >> i) & 1, (cur >> i) & 1);
    }
  }
  last = cur;
}

// ---------- setup / loop ----------

void setup() {
  Serial0.setRxBufferSize(8192);  // a full frame is 27 KB: the default 256 bytes is not enough
  Serial0.begin(SERIAL_BAUD);     // UART0 = CH340 bridge: protocol and logs in one stream
  crc32_init();

  pinMode(PIN_MENU, INPUT_PULLUP);
  pinMode(PIN_EXIT, INPUT_PULLUP);
  pinMode(PIN_ENC_CONF, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);

  epd.init();  // resets, temp_init, clears the glass to white (flashing)
  mode = MODE_SPLASH;
  last_host_frame_ms = millis();

  uint8_t ready[2] = {PROTOCOL_VERSION, mode};
  send_up(CMD_READY, ready, 2);
}

void loop() {
  while (Serial0.available()) feed_byte((uint8_t)Serial0.read());

  // a break mid-frame (cable, lost bytes) - do not hang in a receive phase
  if (rx.phase != RxState::SYNC1 &&
      millis() - rx.last_byte_ms > RX_FRAME_TIMEOUT_MS) {
    Serial0.printf("[rx] frame timeout, resync\n");
    rx.phase = RxState::SYNC1;
  }

  poll_buttons();
  poll_encoder();
  poll_key_debug();

  // fail-safe: the host has been silent longer than HB_TIMEOUT_S - hide
  // the content; the timer is also reset after drawing, so it does not
  // fire every loop iteration
  if (millis() - last_host_frame_ms > HB_TIMEOUT_S * 1000UL) {
    if (mode != MODE_NEUTRAL) draw_neutral_builtin();
    last_host_frame_ms = millis();
  }

  // no delay: a pause here cost lost bytes at 921600 (the buffer fills
  // faster than a loop with delay(2) drains it); the polls are rate-
  // limited by their own timers, everything is non-blocking
  yield();
}
