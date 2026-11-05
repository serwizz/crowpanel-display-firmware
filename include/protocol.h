// Host <-> panel binary protocol, mirrored by the host-side implementation.
// The specification is PROTOCOL.md at the repository root.
// Change both sides together; incompatible changes bump PROTOCOL_VERSION.
#pragma once

#include <stdint.h>

static const uint8_t PROTOCOL_VERSION = 2;

static const uint8_t FRAME_MAGIC[2] = {0xA7, 0x1C};

static const uint16_t EPD_WIDTH = 272;
static const uint16_t EPD_HEIGHT = 792;
static const uint8_t PROTO_STRIDE = EPD_WIDTH / 8;  // 34 protocol bytes per row
static const uint32_t FULL_FRAME_BYTES = (uint32_t)EPD_WIDTH * EPD_HEIGHT / 8;  // 26928

// host -> panel commands
enum Cmd : uint8_t {
  CMD_PING = 0x01,
  CMD_FRAME_FULL = 0x10,
  CMD_FRAME_RECT = 0x11,
  CMD_CLEAR = 0x12,
  CMD_SET_MODE = 0x13,
  CMD_FULL_REFRESH = 0x14,
  CMD_SET_CONFIG = 0x15,
  CMD_HELLO = 0x16,
  CMD_TIME = 0x17,  // hour, minute; sets the panel clock
};

// panel -> host commands
enum CmdUp : uint8_t {
  CMD_PONG = 0x02,
  CMD_KEY_EVENT = 0x80,
  CMD_READY = 0x81,
  CMD_ERROR = 0x82,
  CMD_FRAME_ACK = 0x83,
  CMD_CONFIG_VALUE = 0x84,  // applied value, answers SET_CONFIG
};

// ERROR codes; payload is seq(u8, of the rejected frame) + code(u8)
enum Err : uint8_t {
  ERR_UNKNOWN_CMD = 1,
  ERR_BAD_LENGTH = 2,
  ERR_OUT_OF_BOUNDS = 3,
  ERR_BAD_VALUE = 4,
  ERR_SHADOW_INVALID = 5,  // FRAME_RECT before the first FRAME_FULL after startup
};

// READY capabilities bitmap (features field, u16 LE). A host requires
// the exact PROTOCOL_VERSION match; features only gate optional behavior.
static const uint16_t FTR_ACK = 0x0001;         // FRAME_ACK pacing
static const uint16_t FTR_SHADOW_CRC = 0x0002;  // shadow CRC in PONG
static const uint16_t FTR_TIME = 0x0004;        // TIME command and clock
static const uint16_t FTR_RLE = 0x0008;         // RLE-compressed frames (reserved)

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
// SET_CONFIG clamps hb_timeout into this range; 0 is rejected outright
static const uint32_t HB_TIMEOUT_MIN_S = 30;
static const uint32_t HB_TIMEOUT_MAX_S = 3600;
