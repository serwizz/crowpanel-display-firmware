// Dual SSD1683 cascade driver for the CrowPanel 5.79 panel (272x792).
//
// Factory model (validated on real hardware, see the bring-up sketch):
// - two controllers share the SPI bus; the slave is addressed through
//   the 0x91 0x04 prefix and registers 0xC4/0xC5/0xCE/0xCF/0xA4 (old RAM
//   0x26/0xA6 stays untouched: the factory model drives only the new RAM);
// - a gate line is 100 bytes: bytes 0..49 are the master half, 50..99
//   the slave half; visible X 0..395 maps to master, 396..791 to slave
//   with a +8 column gap in the logical address space (X >= 396 -> +8);
// - data goes out column-major with entry mode 0x05 (Y decrement);
// - full refresh writes ONLY the new RAM (0x24/0xA4) and drives 0x22
//   0xF7. temp_init MUST run before the first full refresh, otherwise
//   the panel loads a garbage waveform and goes all black;
// - the fast 0xDC partial window does not work on this panel (weak BW
//   drive in OTP): the workhorse update is 0xC7 (no load, no flashing,
//   ~1.7 s, slightly grayish black - acceptable for text).
#pragma once

#include <Arduino.h>

#include <stdint.h>

#include "protocol.h"

// Display abstraction: the rest of the firmware talks to the glass
// through this interface only.
class EpdDriver {
 public:
  virtual ~EpdDriver() {}
  virtual void init() = 0;
  // Bitmap: protocol layout, 1 = black, MSB first, row-major, a row is
  // EPD_WIDTH bits = 34 bytes. full = flashing full refresh, otherwise
  // the workhorse (non-flashing) update.
  virtual void drawFull(const uint8_t* bitmap, bool full) = 0;
  virtual void drawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint8_t* bitmap, bool full) = 0;
  virtual void clear(uint8_t colorBlack) = 0;  // 0 = white, 1 = black
  virtual bool busy() = 0;
};

class Ssd1683Epd : public EpdDriver {
 public:
  // flip_long: mirror along the 792-px axis, flip_gate: along the 272-px
  // axis. Set from a marker-frame camera check; defaults follow the
  // factory coordinate model.
  explicit Ssd1683Epd(bool flip_long = false, bool flip_gate = false);

  void init() override;

  // Bitmap: protocol layout, 1 = black, MSB first, row-major, a row is
  // EPD_WIDTH bits = 34 bytes. full = flashing full refresh (0xF7),
  // otherwise the workhorse 0xC7 update.
  void drawFull(const uint8_t* bitmap, bool full) override;

  // Applies the rect to the shadow buffer and refreshes the whole panel
  // (the partial window does not work on this panel). full selects the
  // wave, as in drawFull.
  void drawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                const uint8_t* bitmap, bool full) override;

  // 0 = white, 1 = black. Always a full (flashing) refresh.
  void clear(uint8_t colorBlack) override;

  // Blocking wait for the last wave (called internally after updates).
  void waitIdle();
  bool busy() override;

  // Keeps the shadow buffer in sync (used by the neutral fail-safe path
  // that redraws the same frame).
  const uint8_t* shadow() const { return shadow_; }

 private:
  void wrByte(uint8_t v);
  void wrReg(uint8_t reg);
  void wrData(uint8_t v);
  bool waitBusy(const char* what);
  void hwReset();
  void swReset();
  void tempInit();
  void winMaster(uint8_t xb0, uint8_t xb1, uint16_t ys, uint16_t ye);
  void curMaster(uint8_t xb, uint16_t y);
  void winSlave(uint8_t xb0, uint8_t xb1, uint16_t ys, uint16_t ye);
  void curSlave(uint8_t xb, uint16_t y);
  void writeNewRam();
  void prepWorkUpdate();  // reset + temp cycle, prerequisite for a smooth 0xC7
  void wave(uint8_t seq, const char* what);

  // protocol bitmap (272x792) -> cascade buffer (100 B x 272 lines)
  void compose();

  static const uint16_t GATE_LINES = 272;  // lines per controller
  static const uint8_t SRC_BYTES = 50;     // 400 px / 8 per controller
  static const uint8_t ROW_BYTES = 100;    // cascade bytes per gate line
  static const uint16_t VISIBLE_W = 792;
  static const uint16_t SEAM = 396;
  static const uint32_t IMG_BYTES = (uint32_t)ROW_BYTES * GATE_LINES;  // 27200
  static const uint8_t PROTO_STRIDE = 34;  // 272 / 8, protocol row bytes

  // last shown protocol frame + the cascade layout ready for a send
  uint8_t shadow_[PROTO_STRIDE * EPD_HEIGHT];
  uint8_t img_[IMG_BYTES];

  bool flip_long_;
  bool flip_gate_;
};
