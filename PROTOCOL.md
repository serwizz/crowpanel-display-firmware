# crowpanel-display protocol, version 1

Transport: UART over a CH340 USB bridge, `/dev/ttyUSB0`, 460800 8N1
(921600 is beyond what this CH340 bridge sustains: it drops bytes in a
continuous stream). One port carries everything: binary frames, firmware
debug lines, and text debug commands. A binary frame starts with the
magic `A7 1C` (non-ASCII); a text line starts with a printable byte and
ends with `\n`. Both sides must skip what is not theirs: the host prints
firmware lines, the firmware ignores lines it does not recognize as
commands.

Canonical constants: `include/protocol.h` (C++, this repository); the
host-side implementation mirrors them. Change both sides together and
bump `PROTOCOL_VERSION` on incompatible changes.

## Frame

```
offset  size  field
0       2     magic A7 1C
2       1     cmd
3       2     len, little endian, payload length (0..65535)
5       len   payload
5+len   4     CRC32 (IEEE, zlib.crc32) over cmd + len (2 LE) + payload, little endian
```

On a magic or CRC mismatch the receiver keeps scanning the stream for
the next magic (resynchronization). A full frame is 26928 payload bytes
plus 10 bytes of overhead and fits into a single frame; no chunking is
needed.

## Host -> panel commands

| cmd  | name          | payload                                                        |
|------|---------------|----------------------------------------------------------------|
| 0x01 | PING          | empty; answered with PONG                                      |
| 0x10 | FRAME_FULL    | 26928-byte bitmap, full frame                                  |
| 0x11 | FRAME_RECT    | x,y,w,h (u16 LE) + rect bitmap, partial refresh                |
| 0x12 | CLEAR         | 1 byte: 0 white, 1 black                                       |
| 0x13 | SET_MODE      | 1 byte: 0 SPLASH, 1 DASHBOARD, 2 NEUTRAL                       |
| 0x14 | FULL_REFRESH  | empty; draw the next frame with a full refresh                 |
| 0x15 | SET_CONFIG    | key=value pairs (below), unused for now                        |

SET_CONFIG (reserved for a Wi-Fi evolution): repeating
`key_len(1) key value_len(2 LE) value` tuples. Known keys:
`hb_timeout`, `refresh_interval`, `rotate`.

## Panel -> host commands

| cmd  | name       | payload                                                            |
|------|------------|--------------------------------------------------------------------|
| 0x02 | PONG       | uptime_ms (u32 LE) + mode (u8)                                     |
| 0x81 | READY      | protocol_version (u8) + mode (u8); sent at firmware startup        |
| 0x80 | KEY_EVENT  | key (u8) + event (u8) + value (i16 LE)                             |

key: 1 MENU, 2 EXIT, 3 ENC_UP, 4 ENC_DOWN, 5 ENC_CONF.
event: 0 down, 1 up, 2 short (click), 3 long.
value: accumulated step for the encoder, press duration in ms for long,
0 otherwise.

## Bitmap

- Screen: 272 x 792, 1 bit per pixel, MSB first (bit 7 is the first
  pixel of a row). 1 = black.
- FRAME_FULL: row-major, a row is 272 bits = 34 bytes, 792 x 34 = 26928
  bytes in total.
- FRAME_RECT: every rect row is w bits padded to a byte boundary
  (stride = (w+7)/8), h rows; x+w <= 272, y+h <= 792.
- The black/white inversion this SSD1683 cascade requires is the
  firmware driver's business; the protocol always carries 1 = black.
- Orientation: the origin is the top-left corner of the 272-wide
  portrait layout. Screen rotation is a config key (`rotate`), not a
  different bitmap.

## Refresh policy

The workhorse is partial refresh (FRAME_RECT over small areas). Full
refresh (inversions) is rare: once per N updates, on entering NEUTRAL,
or a long MENU press (the firmware triggers that by itself).
FULL_REFRESH marks the next frame as full.

## Heartbeat and the privacy fail-safe

Firmware: when no valid frame (of any kind) arrives from the host for
`HB_TIMEOUT` seconds (default 120, configurable), the panel draws a
neutral screen by itself and switches to NEUTRAL mode. Host: sends PING
every 30 s while idle. This covers screen lock, host reboots, an
unplugged cable, a hung host process.

## Text debug mode

Lines travel over the same port, terminated by `\n`:

- to the panel: `ping`, `mode <0|1|2>`, `refresh` - the same actions
  without binary encoding;
- from the panel: firmware debug lines (`[epd]`, `[key]`, ... prefixes)
  and the `pong <uptime_ms> <mode>` reply.

ASCII lines never collide with the binary magic, so there is no
ambiguity.

## Sizes and timings (reference)

A full frame over UART at 460800 takes about 0.6 s. A partial refresh
of the panel takes 0.5-1 s, a full refresh several seconds. A complete
button -> host -> frame round trip is 1-2 s.
