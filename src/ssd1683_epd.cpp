#include "ssd1683_epd.h"

#include <string.h>

// ---------- pins (Elecrow wiki, confirmed on a live panel) ----------

static const int PIN_EPD_SCK = 12;
static const int PIN_EPD_MOSI = 11;
static const int PIN_EPD_CS = 45;
static const int PIN_EPD_DC = 46;
static const int PIN_EPD_RES = 47;
static const int PIN_EPD_BUSY = 48;
static const int PIN_EPD_PWR = 7;  // panel power, HIGH before init

// ---------- update sequences (Display Update Control 2, reg 0x22) ----------

static const uint8_t SEQ_FULL = 0xF7;  // full flashing refresh, ~2.3 s
static const uint8_t SEQ_WORK = 0xC7;  // workhorse: no flash, ~1.7 s

static const uint32_t BUSY_TIMEOUT_MS = 60000;

Ssd1683Epd::Ssd1683Epd(bool flip_long, bool flip_gate)
    : flip_long_(flip_long), flip_gate_(flip_gate) {}

// ---------- low level: bit-bang SPI (as in the Elecrow spi.cpp) ----------

void Ssd1683Epd::wrByte(uint8_t dat) {
  digitalWrite(PIN_EPD_CS, LOW);
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(PIN_EPD_SCK, LOW);
    digitalWrite(PIN_EPD_MOSI, (dat & 0x80) ? HIGH : LOW);
    digitalWrite(PIN_EPD_SCK, HIGH);
    dat <<= 1;
  }
  digitalWrite(PIN_EPD_CS, HIGH);
}

void Ssd1683Epd::wrReg(uint8_t reg) {
  digitalWrite(PIN_EPD_DC, LOW);
  wrByte(reg);
  digitalWrite(PIN_EPD_DC, HIGH);
}

void Ssd1683Epd::wrData(uint8_t v) { wrByte(v); }

bool Ssd1683Epd::waitBusy(const char* what) {
  uint32_t t0 = millis();
  while (digitalRead(PIN_EPD_BUSY) == HIGH) {
    if (millis() - t0 > BUSY_TIMEOUT_MS) {
      Serial0.printf("[epd] BUSY timeout (%s)\n", what);
      return false;
    }
    yield();
  }
  uint32_t dt = millis() - t0;
  if (dt > 1) Serial0.printf("[epd] BUSY (%s): %lu ms\n", what, (unsigned long)dt);
  return true;
}

// ---------- reset and the temperature fix ----------

void Ssd1683Epd::hwReset() {
  delay(10);
  digitalWrite(PIN_EPD_RES, LOW);
  delay(10);
  digitalWrite(PIN_EPD_RES, HIGH);
  delay(10);
  waitBusy("hw_reset");
}

void Ssd1683Epd::swReset() {
  wrReg(0x12);
  waitBusy("swreset");
}

// Factory FastMode1Init: temperature sensor -> temperature register.
// MUST run before the first full refresh: without it the controller loads
// a garbage waveform and the panel goes all black (hardware-verified bug).
void Ssd1683Epd::tempInit() {
  wrReg(0x18);
  wrData(0x80);
  wrReg(0x22);
  wrData(0xB1);
  wrReg(0x20);
  waitBusy("temp_load");
  wrReg(0x1A);  // OTP waveform temperature bucket
  wrData(0x64);
  wrData(0x00);
  wrReg(0x22);
  wrData(0x91);
  wrReg(0x20);
  waitBusy("temp_write");
  wrReg(0x3C);
  wrData(0x03);
}

// ---------- window / cursor (factory cascade model) ----------

void Ssd1683Epd::winMaster(uint8_t xb0, uint8_t xb1, uint16_t ys, uint16_t ye) {
  wrReg(0x11);  // data entry mode 0x05: X increment, Y decrement
  wrData(0x05);
  wrReg(0x44);
  wrData(xb0);
  wrData(xb1);
  wrReg(0x45);
  wrData(ys & 0xFF);
  wrData(ys >> 8);
  wrData(ye & 0xFF);
  wrData(ye >> 8);
}

void Ssd1683Epd::curMaster(uint8_t xb, uint16_t y) {
  wrReg(0x4E);
  wrData(xb);
  wrReg(0x4F);
  wrData(y & 0xFF);
  wrData(y >> 8);
}

// The slave is addressed through the 0x91 0x04 prefix - mandatory.
void Ssd1683Epd::winSlave(uint8_t xb0, uint8_t xb1, uint16_t ys, uint16_t ye) {
  wrReg(0x91);
  wrData(0x04);
  wrReg(0xC4);
  wrData(xb0);
  wrData(xb1);
  wrReg(0xC5);
  wrData(ys & 0xFF);
  wrData(ys >> 8);
  wrData(ye & 0xFF);
  wrData(ye >> 8);
}

void Ssd1683Epd::curSlave(uint8_t xb, uint16_t y) {
  wrReg(0xCE);
  wrData(xb);
  wrReg(0xCF);
  wrData(y & 0xFF);
  wrData(y >> 8);
}

// ---------- frame upload: only the new RAM (0x24 master / 0xA4 slave) ----------

void Ssd1683Epd::writeNewRam() {
  uint32_t t0 = millis();
  winMaster(0, SRC_BYTES - 1, GATE_LINES - 1, 0);
  curMaster(0, GATE_LINES - 1);
  wrReg(0x24);
  for (uint8_t col = 0; col < SRC_BYTES; col++)
    for (uint16_t y = 0; y < GATE_LINES; y++)
      wrData(img_[(uint32_t)y * ROW_BYTES + col]);
  winSlave(SRC_BYTES - 1, 0, GATE_LINES - 1, 0);
  curSlave(SRC_BYTES - 1, GATE_LINES - 1);
  wrReg(0xA4);
  for (uint8_t col = SRC_BYTES; col < ROW_BYTES; col++)
    for (uint16_t y = 0; y < GATE_LINES; y++)
      wrData(img_[(uint32_t)y * ROW_BYTES + col]);
  Serial0.printf("[epd] ram upload: %lu ms\n", (unsigned long)(millis() - t0));
}

void Ssd1683Epd::wave(uint8_t seq, const char* what) {
  wrReg(0x22);
  wrData(seq);
  wrReg(0x20);
  waitBusy(what);
}

// ---------- protocol bitmap -> cascade layout ----------
//
// Protocol: portrait 272x792, row-major: a bitmap row is one protocol Y
// (792 rows of 34 bytes), bit 1 = black, bit index = protocol X.
// Cascade: a gate line (0..271) is 100 bytes over the long axis X
// (0..791 visible, +8 gap at the seam), RAM bit 1 = white (inverted).
// Mapping: protocol Y runs along the long axis X, protocol X along the
// gate lines.
void Ssd1683Epd::compose() {
  uint32_t t0 = millis();
  for (uint16_t x = 0; x < VISIBLE_W; x++) {
    // bitmap row for this long-axis position: protocol Y
    uint16_t py = flip_long_ ? (EPD_HEIGHT - 1 - x) : x;
    const uint8_t* row = shadow_ + (size_t)py * PROTO_STRIDE;
    uint16_t xa = (x >= SEAM) ? x + 8 : x;
    uint8_t m = 0x80 >> (xa & 7);
    for (uint16_t gy = 0; gy < GATE_LINES; gy++) {
      uint16_t px = flip_gate_ ? (EPD_WIDTH - 1 - gy) : gy;  // protocol X
      bool black = row[px >> 3] & (0x80 >> (px & 7));
      uint8_t* b = &img_[(uint32_t)gy * ROW_BYTES + (xa >> 3)];
      if (black)
        *b &= ~m;  // RAM 0 = black
      else
        *b |= m;
    }
  }
  Serial0.printf("[epd] compose: %lu ms\n", (unsigned long)(millis() - t0));
}

// ---------- public API ----------

void Ssd1683Epd::init() {
  uint32_t t0 = millis();
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, HIGH);  // panel power before init
  delay(10);
  pinMode(PIN_EPD_SCK, OUTPUT);
  pinMode(PIN_EPD_MOSI, OUTPUT);
  pinMode(PIN_EPD_RES, OUTPUT);
  pinMode(PIN_EPD_DC, OUTPUT);
  pinMode(PIN_EPD_CS, OUTPUT);
  pinMode(PIN_EPD_BUSY, INPUT);

  hwReset();
  swReset();
  tempInit();  // without this the first full refresh paints black

  memset(shadow_, 0, sizeof(shadow_));  // 0 = white in the protocol
  compose();
  writeNewRam();
  wave(SEQ_FULL, "init_clear");  // start from a clean white panel
  Serial0.printf("[epd] init done: %lu ms\n", (unsigned long)(millis() - t0));
}

// The workhorse 0xC7 update only runs smooth (no flashing, ~1.7 s) right
// after a reset + temperature cycle; without them the controller falls
// back to a flashing full-like drive (measured: whole-screen inversions,
// 2.26 s). The factory UI does reset+temp before every menu redraw.
void Ssd1683Epd::prepWorkUpdate() {
  hwReset();
  tempInit();
}

void Ssd1683Epd::drawFull(const uint8_t* bitmap, bool full) {
  memcpy(shadow_, bitmap, (size_t)PROTO_STRIDE * EPD_HEIGHT);
  if (!full) prepWorkUpdate();
  compose();
  writeNewRam();
  wave(full ? SEQ_FULL : SEQ_WORK, full ? "full_refresh" : "work_update");
}

void Ssd1683Epd::drawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint8_t* bitmap, bool full) {
  if (!full) prepWorkUpdate();
  uint16_t stride = (w + 7) / 8;
  for (uint16_t r = 0; r < h; r++) {
    uint8_t* srow = shadow_ + (size_t)(y + r) * PROTO_STRIDE;
    const uint8_t* brow = bitmap + (size_t)r * stride;
    for (uint16_t c = 0; c < w; c++) {
      bool black = brow[c >> 3] & (0x80 >> (c & 7));
      uint8_t m = 0x80 >> ((x + c) & 7);
      if (black)
        srow[(x + c) >> 3] |= m;
      else
        srow[(x + c) >> 3] &= ~m;
    }
  }
  compose();
  writeNewRam();
  wave(full ? SEQ_FULL : SEQ_WORK, full ? "full_refresh" : "work_update");
}

void Ssd1683Epd::clear(uint8_t colorBlack) {
  memset(shadow_, colorBlack ? 0xFF : 0x00, sizeof(shadow_));
  compose();
  writeNewRam();
  wave(SEQ_FULL, "clear");  // full flashing drive in both directions
}

void Ssd1683Epd::waitIdle() { waitBusy("idle"); }

bool Ssd1683Epd::busy() { return digitalRead(PIN_EPD_BUSY) == HIGH; }
