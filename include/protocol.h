// Host <-> panel binary protocol, mirrored by the host-side implementation.
// The specification is PROTOCOL.md at the repository root.
// Change both sides together; incompatible changes bump PROTOCOL_VERSION.
#pragma once

#include <stdint.h>

static const uint8_t PROTOCOL_VERSION = 1;

static const uint8_t FRAME_MAGIC[2] = {0xA7, 0x1C};

static const uint16_t EPD_WIDTH = 272;
static const uint16_t EPD_HEIGHT = 792;
static const uint32_t FULL_FRAME_BYTES = (uint32_t)EPD_WIDTH * EPD_HEIGHT / 8;  // 26928
static const uint16_t MAX_PAYLOAD = 0xFFFF;

// host -> panel commands
enum Cmd : uint8_t {
  CMD_PING = 0x01,
  CMD_FRAME_FULL = 0x10,
  CMD_FRAME_RECT = 0x11,
  CMD_CLEAR = 0x12,
  CMD_SET_MODE = 0x13,
  CMD_FULL_REFRESH = 0x14,
  CMD_SET_CONFIG = 0x15,
};

// panel -> host commands
enum CmdUp : uint8_t {
  CMD_PONG = 0x02,
  CMD_KEY_EVENT = 0x80,
  CMD_READY = 0x81,
};

enum Mode : uint8_t { MODE_SPLASH = 0, MODE_DASHBOARD = 1, MODE_NEUTRAL = 2 };

enum Key : uint8_t {
  KEY_NONE = 0,
  KEY_MENU = 1,
  KEY_EXIT = 2,
  KEY_ENC_UP = 3,
  KEY_ENC_DOWN = 4,
  KEY_ENC_CONF = 5,
};

enum KeyEvent : uint8_t { EV_DOWN = 0, EV_UP = 1, EV_SHORT = 2, EV_LONG = 3 };

// Heartbeat fail-safe: with no valid frame from the host for this many
// seconds the panel draws a neutral screen by itself (privacy must not
// depend on the host being alive).
static const uint32_t HB_TIMEOUT_S = 120;
