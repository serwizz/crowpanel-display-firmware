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

static void send_pong(uint8_t seq);
static bool set_mode(uint8_t m);
static void draw_neutral_builtin(const char* why, bool live);
static bool handle_frame(uint8_t cmd, const uint8_t* p, uint16_t len, uint8_t seq);
static void send_error(uint8_t seq, uint8_t code);
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

static uint8_t frame_buf[FULL_FRAME_BYTES];  // payload buffer: a full frame is the largest command payload

static uint8_t up_seq = 0;  // autonomous upstream frames counter
static uint8_t next_seq() { return ++up_seq ? up_seq : ++up_seq; }

// FRAME_ACK pacing state: the host waits for FRAME_ACK(seq) before the
// next frame; ack_enabled exists so the text debug can simulate a lost
// ACK ("ackoff"/"ackon") and exercise the host fallback
static bool ack_enabled = true;
static uint8_t last_drawn_seq = 0;  // echoed in PONG for the lost-ACK fallback

// seq: responses to a request echo its seq; autonomous frames (boot
// READY, KEY_EVENT) take the next value of the panel-side counter
static void send_up(uint8_t cmd, uint8_t seq, const uint8_t* payload, uint16_t len) {
  uint8_t head[6] = {FRAME_MAGIC[0], FRAME_MAGIC[1], cmd, seq, (uint8_t)(len & 0xFF),
                     (uint8_t)(len >> 8)};
  Serial0.write(head, 6);
  if (len) Serial0.write(payload, len);
  uint8_t body_for_crc[4] = {cmd, seq, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  uint32_t crc = crc32_update(body_for_crc, 4, 0xFFFFFFFFu);
  if (len) crc = crc32_update(payload, len, crc);
  crc ^= 0xFFFFFFFFu;
  uint8_t crc_le[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16),
                       (uint8_t)(crc >> 24)};
  Serial0.write(crc_le, 4);
}

static void send_key(uint8_t key, uint8_t event, int16_t value) {
  uint8_t p[4] = {key, event, (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF)};
  send_up(CMD_KEY_EVENT, next_seq(), p, 4);
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
static bool shadow_valid = false;  // true after the first FRAME_FULL after startup
static bool clock_known = false;   // set by TIME; the panel has no RTC
static bool clock_stale = false;   // drawn by the heartbeat timeout (colon hidden)
static uint8_t clock_h = 0, clock_m = 0;
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

static bool set_mode(uint8_t m) {
  if (m > MODE_NEUTRAL) {
    Serial0.printf("[rx] SET_MODE out of range: %u\n", (unsigned)m);
    return false;
  }
  if (m == MODE_NEUTRAL) {
    if (mode == MODE_NEUTRAL) return true;  // already there: no second wave
    // switching to NEUTRAL must replace the content right away: the
    // heartbeat timer only draws while mode != NEUTRAL, so without an
    // immediate draw the old content would stay on the glass for good
    draw_neutral_builtin("host set NEUTRAL", true);
    return true;
  }
  mode = m;
  return true;
}

// ---------- receive: text debug mode ----------

static char text_line[96];
static uint8_t text_len = 0;

static void handle_text_line(char* line) {
  if (!strcmp(line, "ping")) {
    // text mode is for humans at a terminal: answer with a readable
    // line, not a binary frame (binary PONG answers binary PING only)
    Serial0.printf("[pong] uptime=%lu ms mode=%u shadow_crc=0x%08lx\n",
                   (unsigned long)millis(), (unsigned)mode,
                   (unsigned long)crc32_calc(epd.shadow(), FULL_FRAME_BYTES));
  } else if (!strncmp(line, "mode ", 5)) {
    set_mode(atoi(line + 5));
  } else if (!strncmp(line, "time ", 5)) {
    int h = -1, m = -1;
    char tail = 0;
    // %c in the format catches trailing garbage: "23:45x" must not pass
    if (sscanf(line + 5, "%d:%d%c", &h, &m, &tail) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
      bool changed = !clock_known || clock_h != (uint8_t)h || clock_m != (uint8_t)m || clock_stale;
      clock_h = (uint8_t)h; clock_m = (uint8_t)m; clock_known = true;
      if (mode == MODE_NEUTRAL && changed) draw_neutral_builtin("time update", true);
    } else {
      Serial0.printf("[rx] bad time: %s\n", line + 5);
    }
  } else if (!strcmp(line, "refresh")) {
    force_full_next = true;
    Serial0.printf("[epd] full refresh on next frame\n");
  } else if (!strcmp(line, "ackoff")) {
    ack_enabled = false;
    Serial0.printf("[epd] FRAME_ACK suppressed (ackoff)\n");
  } else if (!strcmp(line, "ackon")) {
    ack_enabled = true;
    Serial0.printf("[epd] FRAME_ACK enabled (ackon)\n");
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
  enum Phase { SYNC1, SYNC2, CMD, SEQ, LEN1, LEN2, PAYLOAD, CRC1, CRC2, CRC3, CRC4, SKIP };
  Phase phase = SYNC1;
  uint8_t cmd = 0;
  uint8_t seq = 0;
  uint16_t len = 0;
  uint16_t got = 0;
  uint32_t skip = 0;  // bytes to discard after a length rejection
  uint8_t hdr[4];  // cmd + seq + len(2) - start of the frame body for CRC
  uint32_t crc_got = 0;
  uint32_t last_byte_ms = 0;  // receive timeout: a break mid-frame
};

static const uint32_t RX_FRAME_TIMEOUT_MS = 500;

static RxState rx;
static uint32_t last_host_frame_ms = 0;  // heartbeat fail-safe
static uint32_t hb_timeout_s = HB_TIMEOUT_S;  // SET_CONFIG clamps it to HB_TIMEOUT_MIN_S..MAX_S

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
      rx.cmd = b; rx.hdr[0] = b; rx.phase = RxState::SEQ; return;
    case RxState::SEQ:
      rx.seq = b; rx.hdr[1] = b; rx.phase = RxState::LEN1; return;
    case RxState::LEN1:
      rx.len = b; rx.hdr[2] = b; rx.phase = RxState::LEN2; return;
    case RxState::LEN2: {
      rx.len |= ((uint16_t)b) << 8; rx.hdr[3] = b;
      // a full frame is the largest command payload; a longer claim
      // means the peers disagree on the frame size - a configuration
      // mismatch that must be visible, not silent. ERROR echoes the
      // seq from an unverified header; that is harmless. The payload
      // is not read: skip exactly len+4 bytes (payload and its CRC)
      // so a magic-like sequence inside cannot spawn a phantom frame
      // that swallows the next real one
      if (rx.len > FULL_FRAME_BYTES) {
        send_error(rx.seq, ERR_BAD_LENGTH);
        rx.skip = (uint32_t)rx.len + 4;
        rx.phase = RxState::SKIP;
        return;
      }
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
      uint32_t crc = crc32_update(rx.hdr, 4, 0xFFFFFFFFu);
      if (rx.got) crc = crc32_update(frame_buf, rx.got, crc);
      crc ^= 0xFFFFFFFFu;
      if (crc == rx.crc_got) {
        if (handle_frame(rx.cmd, frame_buf, rx.len, rx.seq))
          last_host_frame_ms = millis();  // accepted frames only
      } else {
        Serial0.printf("[rx] bad CRC, frame dropped\n");
      }
      rx.phase = RxState::SYNC1;
      return;
    }
    case RxState::SKIP:
      if (--rx.skip == 0) rx.phase = RxState::SYNC1;
      return;
    default:
      rx.phase = RxState::SYNC1;
  }
}

// ---------- host command handlers ----------

static void send_pong(uint8_t seq) {
  uint32_t up = millis();
  uint32_t shc = crc32_calc(epd.shadow(), FULL_FRAME_BYTES);
  uint8_t p[10] = {(uint8_t)up, (uint8_t)(up >> 8), (uint8_t)(up >> 16),
                   (uint8_t)(up >> 24), mode, last_drawn_seq,
                   (uint8_t)shc, (uint8_t)(shc >> 8), (uint8_t)(shc >> 16), (uint8_t)(shc >> 24)};
  send_up(CMD_PONG, seq, p, 10);
}

// READY v2: version + features (u16 LE) + shadow_valid
static void send_ready(uint8_t seq) {
  uint8_t p[4] = {PROTOCOL_VERSION, (uint8_t)((FTR_ACK | FTR_SHADOW_CRC | FTR_TIME) & 0xFF),
                  (uint8_t)((FTR_ACK | FTR_SHADOW_CRC | FTR_TIME) >> 8), (uint8_t)shadow_valid};
  send_up(CMD_READY, seq, p, 4);
}

// sent right after the wave of a drawn frame completes; the host paces
// its sends on this, which is what protects the RX ring during a wave
static void send_frame_ack(uint8_t seq) {
  if (!ack_enabled) return;
  send_up(CMD_FRAME_ACK, seq, &seq, 1);
}

// one ERROR per rejected binary frame; the text mode gets none (it has
// no seq and its garbage lines are legitimate resynchronization)
static void send_error(uint8_t seq, uint8_t code) {
  uint8_t p[2] = {seq, code};
  send_up(CMD_ERROR, seq, p, 2);
}

// returns true when the frame was accepted; only accepted frames feed
// the heartbeat fail-safe - a host spamming rejected frames must not
// hold the panel off its privacy neutral screen
static bool handle_frame(uint8_t cmd, const uint8_t* p, uint16_t len, uint8_t seq) {
  switch (cmd) {
    case CMD_PING:
      if (len != 0) { send_error(seq, ERR_BAD_LENGTH); return false; }
      send_pong(seq);
      return true;
    case CMD_HELLO:
      if (len != 1) { send_error(seq, ERR_BAD_LENGTH); return false; }
      send_ready(seq);
      return true;
    case CMD_FRAME_FULL:
      if (len != FULL_FRAME_BYTES) {
        Serial0.printf("[rx] FRAME_FULL wrong size: %u\n", (unsigned)len);
        send_error(seq, ERR_BAD_LENGTH);
        return false;
      }
      epd.drawFull(p, take_full_refresh());
      shadow_valid = true;  // the host shadow can now be trusted
      mode = MODE_DASHBOARD;
      last_drawn_seq = seq;
      send_frame_ack(seq);
      return true;
    case CMD_FRAME_RECT:
      if (len < 8) { send_error(seq, ERR_BAD_LENGTH); return false; }
      if (!shadow_valid) {
        // no FRAME_FULL since boot yet: there is no shadow base to
        // apply the rect to (host must send a full frame first)
        send_error(seq, ERR_SHADOW_INVALID);
        return false;
      }
      {
        uint16_t x = p[0] | (p[1] << 8), y = p[2] | (p[3] << 8);
        uint16_t w = p[4] | (p[5] << 8), h = p[6] | (p[7] << 8);
        uint32_t expect = 8 + (uint32_t)((w + 7) / 8) * h;
        if (w == 0 || h == 0 || x + w > EPD_WIDTH || y + h > EPD_HEIGHT) {
          Serial0.printf("[rx] FRAME_RECT out of bounds: %u,%u %ux%u\n",
                         (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
          send_error(seq, ERR_OUT_OF_BOUNDS);
          return false;
        }
        if (len != expect) {
          Serial0.printf("[rx] FRAME_RECT size %u != %u (stride*h)\n",
                         (unsigned)len, (unsigned)expect);
          send_error(seq, ERR_BAD_LENGTH);
          return false;
        }
        // a rect over half the screen is worth a clean full refresh; either way
        // the full-refresh period is anchored to the last actually full wave
        bool full;
        if ((uint32_t)w * h >= RECT_FULL_THRESHOLD) {
          frames_since_full = 0;
          force_full_next = false;  // this wave is the full one
          full = true;
        } else {
          full = take_full_refresh();
        }
        epd.drawRect(x, y, w, h, p + 8, full);
        mode = MODE_DASHBOARD;
        last_drawn_seq = seq;
        send_frame_ack(seq);
        return true;
      }
    case CMD_CLEAR:
      if (len != 1) { send_error(seq, ERR_BAD_LENGTH); return false; }
      epd.clear(p[0]);
      last_drawn_seq = seq;
      send_frame_ack(seq);
      return true;
    case CMD_SET_MODE:
      if (len != 1) { send_error(seq, ERR_BAD_LENGTH); return false; }
      if (!set_mode(p[0])) { send_error(seq, ERR_BAD_VALUE); return false; }
      return true;
    case CMD_FULL_REFRESH:
      if (len != 0) { send_error(seq, ERR_BAD_LENGTH); return false; }
      force_full_next = true;
      return true;
    case CMD_SET_CONFIG: {
      // tuples: key_len(1) key value_len(2 LE) value. Known keys:
      // hb_timeout (ASCII-decimal seconds, clamped to HB_TIMEOUT_MIN_S..
      // HB_TIMEOUT_MAX_S; 0, non-numbers and overflows are rejected and
      // leave the previous value). Each successful tuple is answered
      // with CONFIG_VALUE carrying the applied u32.
      if (len == 0) { send_error(seq, ERR_BAD_LENGTH); return false; }
      uint16_t pos = 0;
      while (pos < len) {
        uint8_t klen = p[pos++];
        if (len - pos < (uint16_t)klen + 2) { send_error(seq, ERR_BAD_LENGTH); return false; }
        const uint8_t* key = p + pos;
        pos += klen;
        uint16_t vlen = p[pos] | ((uint16_t)p[pos + 1] << 8);
        pos += 2;
        if (len - pos < vlen) { send_error(seq, ERR_BAD_LENGTH); return false; }
        const uint8_t* val = p + pos;
        pos += vlen;
        if (klen != 10 || memcmp(key, "hb_timeout", 10) != 0) {
          Serial0.printf("[rx] SET_CONFIG unknown key (%u bytes)\n", (unsigned)klen);
          send_error(seq, ERR_BAD_VALUE);
          return false;
        }
        if (vlen == 0 || vlen > 10) { send_error(seq, ERR_BAD_VALUE); return false; }
        uint32_t v = 0;
        bool bad = false;
        for (uint16_t i = 0; i < vlen && !bad; i++) {
          if (val[i] < '0' || val[i] > '9') { bad = true; break; }
          uint32_t d = val[i] - '0';
          if (v > (UINT32_MAX - d) / 10) { bad = true; break; }  // u32 overflow
          v = v * 10 + d;
        }
        if (bad || v == 0) { send_error(seq, ERR_BAD_VALUE); return false; }
        if (v < HB_TIMEOUT_MIN_S) v = HB_TIMEOUT_MIN_S;
        if (v > HB_TIMEOUT_MAX_S) v = HB_TIMEOUT_MAX_S;
        hb_timeout_s = v;
        uint8_t resp[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        send_up(CMD_CONFIG_VALUE, seq, resp, 4);
      }
      return true;
    }
    case CMD_TIME:
      // state-only, no ACK (like SET_MODE). The value is
      // stored in any mode; in NEUTRAL it redraws so the clock ticks
      // (skipped when the value did not change)
      if (len != 2) { send_error(seq, ERR_BAD_LENGTH); return false; }
      if (p[0] > 23 || p[1] > 59) { send_error(seq, ERR_BAD_VALUE); return false; }
      {
        bool changed = !clock_known || clock_h != p[0] || clock_m != p[1] || clock_stale;
        clock_h = p[0]; clock_m = p[1]; clock_known = true;
        if (mode == MODE_NEUTRAL && changed) draw_neutral_builtin("time update", true);
      }
      return true;
    default:
      Serial0.printf("[rx] unknown command 0x%02x\n", cmd);
      send_error(seq, ERR_UNKNOWN_CMD);
      return false;
  }
}

// ---------- neutral screen (privacy fail-safe) ----------

// Same intent as neutral_frame() on the host: no personal data. White
// screen plus a big HH:MM once the host has set the clock (TIME); the
// panel has no RTC, so at a heartbeat timeout the clock freezes at the
// last accepted value (documented v2 limitation, PROTOCOL.md).

// 3x5 font, one font pixel is an 8x8 block; rows top-down, bit 2 = left
// column, 1 = ink. Index 10 is the colon.
static const uint8_t GLYPH3X5[11][5] = {
    {0x07, 0x05, 0x05, 0x05, 0x07},  // 0
    {0x02, 0x06, 0x02, 0x02, 0x07},  // 1
    {0x07, 0x01, 0x07, 0x04, 0x07},  // 2
    {0x07, 0x01, 0x07, 0x01, 0x07},  // 3
    {0x05, 0x05, 0x07, 0x01, 0x01},  // 4
    {0x07, 0x04, 0x07, 0x01, 0x07},  // 5
    {0x07, 0x04, 0x07, 0x05, 0x07},  // 6
    {0x07, 0x01, 0x01, 0x02, 0x02},  // 7
    {0x07, 0x05, 0x07, 0x05, 0x07},  // 8
    {0x07, 0x05, 0x07, 0x01, 0x07},  // 9
    {0x00, 0x02, 0x00, 0x02, 0x00},  // :
};

// x is a multiple of 8: every 8x8 block covers exactly one byte per row
static void draw_glyph(uint8_t* buf, uint8_t n, int16_t x, int16_t y) {
  if (n == 10) {  // colon: two 8x8 squares
    for (uint8_t r = 0; r < 5; r++)
      if (GLYPH3X5[10][r])
        for (uint8_t dy = 0; dy < 8; dy++)
          buf[(size_t)(y + r * 8 + dy) * PROTO_STRIDE + (x >> 3)] = 0xFF;
    return;
  }
  for (uint8_t r = 0; r < 5; r++)
    for (uint8_t c = 0; c < 3; c++)
      if (GLYPH3X5[n][r] & (4 >> c))
        for (uint8_t dy = 0; dy < 8; dy++)
          buf[(size_t)(y + r * 8 + dy) * PROTO_STRIDE + ((x + c * 8) >> 3)] = 0xFF;
}

// live=false hides the colon: a heartbeat-timeout neutral screen shows
// HH MM, marking the time as stale (the panel has no RTC and the host
// went silent). A TIME update redraws with the colon.
static void compose_clock(uint8_t* buf, bool live) {
  const uint8_t glyphs[5] = {(uint8_t)(clock_h / 10), (uint8_t)(clock_h % 10), 10,
                             (uint8_t)(clock_m / 10), (uint8_t)(clock_m % 10)};
  int16_t x = 64;  // 4 digits of 24 + colon 8 + 4 gaps of 8 = 136, (272-136)/2 = 68 -> 64 keeps bytes aligned
  for (uint8_t i = 0; i < 5; i++) {
    if (glyphs[i] != 10 || live) draw_glyph(buf, glyphs[i], x, 376);  // (792-40)/2
    x += (glyphs[i] == 10) ? 8 : 24;
    x += 8;
  }
}

static void draw_neutral_builtin(const char* why, bool live) {
  Serial0.printf("[fail-safe] %s - drawing neutral screen\n", why);
  memset(frame_buf, 0, FULL_FRAME_BYTES);  // 0 = white (protocol: 1 = black)
  if (clock_known) compose_clock(frame_buf, live);
  clock_stale = !live;  // a heartbeat-timeout clock must come back on the next TIME
  epd.drawFull(frame_buf, true);           // neutral always drives full
  frames_since_full = 0;  // a neutral wave is a full wave: anchor the period
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

// ---------- setup / loop ----------

void setup() {
  Serial0.setRxBufferSize(8192);  // a full frame is 27 KB: the default 256 bytes is not enough;
                                  // the ring covers ~178 ms of traffic at 460800, and a wave blocks
                                  // the loop for ~2.3 s - the host paces frames by waiting for
                                  // FRAME_ACK, which is what actually protects this buffer
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

  send_ready(next_seq());  // boot READY: shadow_valid = 0, no FRAME_FULL yet
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

  // fail-safe: the host has been silent longer than hb_timeout_s (default
  // HB_TIMEOUT_S, SET_CONFIG clamps it to HB_TIMEOUT_MIN_S..MAX_S) - hide
  // the content; the timer is also reset after drawing, so it does not
  // fire every loop iteration
  if (millis() - last_host_frame_ms > hb_timeout_s * 1000UL) {
    if (mode != MODE_NEUTRAL) draw_neutral_builtin("no heartbeat", false);
    last_host_frame_ms = millis();
  }

  // no delay: a wave blocks the loop for ~2.3 s while the 8 KB RX ring
  // keeps filling (it covers only ~178 ms of traffic at 460800), so any
  // pause here costs bytes; the polls are rate-limited by their own
  // timers, everything is non-blocking
  yield();
}
