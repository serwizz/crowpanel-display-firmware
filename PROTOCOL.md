# crowpanel-display protocol, version 2

Transport: UART over a CH340 USB bridge, `/dev/ttyUSB0`, 460800 8N1
(921600 is beyond what this CH340 bridge sustains: it drops bytes in a
continuous stream). One port carries everything: binary frames, firmware
debug lines, and text debug commands. A binary frame starts with the
magic `A7 1C` (non-ASCII); a text line starts with a printable byte and
ends with `\n`. Both sides must skip what is not theirs: the host prints
firmware lines, the firmware ignores lines it does not recognize as
commands. The frame format is transport-neutral: the same bytes work
over TCP for a Wi-Fi evolution.

Canonical constants: `include/protocol.h` (C++, this repository); the
host-side implementation mirrors them. Change both sides together and
bump `PROTOCOL_VERSION` on incompatible changes.

## Frame

```
offset  size  field
0       2     magic A7 1C
2       1     cmd
3       1     seq
4       2     len, little endian, payload length (0..65535)
6       len   payload
6+len   4     CRC32 (IEEE, zlib.crc32) over cmd + seq + len (2 LE) + payload, little endian
```

On a magic or CRC mismatch the receiver keeps scanning the stream for
the next magic (resynchronization). A full frame is 26928 payload bytes
plus 11 bytes of overhead and fits into a single frame; no chunking is
needed.

## Sequence numbers

seq is present in every binary frame of both sides. The host counts its
outgoing frames from 1, skipping 0 on an 8-bit overflow. A response
(PONG, READY, later ERROR/FRAME_ACK/CONFIG_VALUE) echoes the seq of the
request it answers. Autonomous panel frames (the boot READY, KEY_EVENT)
use the panel-side counter. Text-mode lines are not frames and carry no
seq at all; the binary PONG that answers a text `ping` goes out with
seq = 0.

## Handshake

After opening the port the host sends `HELLO` with its protocol version;
the panel answers `READY` (the same payload as the boot READY). The
panel also sends `READY` by itself right after firmware startup. Any
READY frame within the handshake window completes the handshake - the
echo to HELLO or an autonomous boot READY. The host requires the
versions to match exactly and fails with a clear error otherwise
(`protocol version mismatch: host 2, panel N`): both sides ship
together, so a version gap means a broken setup, not a feature
negotiation.

`features` is a capability bitmap: 0x0001 ACK (FRAME_ACK pacing), 0x0002
SHADOW_CRC (shadow CRC32 in PONG), 0x0004 TIME (clock on the neutral
screen), 0x0008 RLE (reserved, not activated until it proves itself on
real frames). `shadow_valid` is 0 until the first FRAME_FULL after
startup (a RECT before that has no base to apply to), 1 after.

## Host -> panel commands

| cmd  | name          | payload                                                        |
|------|---------------|----------------------------------------------------------------|
| 0x01 | PING          | empty; answered with PONG                                      |
| 0x10 | FRAME_FULL    | 26928-byte bitmap, full frame                                  |
| 0x11 | FRAME_RECT    | x,y,w,h (u16 LE) + rect bitmap, partial refresh                |
| 0x12 | CLEAR         | 1 byte: 0 white, 1 black                                       |
| 0x13 | SET_MODE      | 1 byte: 0 SPLASH, 1 DASHBOARD, 2 NEUTRAL (draws the neutral screen at once) |
| 0x14 | FULL_REFRESH  | empty; draw the next frame with a full refresh                 |
| 0x15 | SET_CONFIG    | key=value pairs (below), unused for now                        |
| 0x16 | HELLO         | protocol version (u8); answered with READY                     |
| 0x17 | TIME          | reserved (stage 6): hour, minute                               |

SET_CONFIG (reserved for a Wi-Fi evolution): repeating
`key_len(1) key value_len(2 LE) value` tuples. Known keys:
`hb_timeout`, `refresh_interval`, `rotate`.

## Panel -> host commands

| cmd  | name       | payload                                                            |
|------|------------|--------------------------------------------------------------------|
| 0x02 | PONG       | uptime_ms (u32 LE) + mode (u8)                                     |
| 0x81 | READY      | protocol_version (u8) + features (u16 LE) + shadow_valid (u8); at startup and on HELLO |
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

Every update redraws the whole screen: the fast partial modes of this
panel cannot drive black (see notes/waves.md), so FRAME_RECT only
limits what is recomputed, not what is refreshed. Smooth updates (no
flashing, ~1.7 s) carry regular frames. A full flashing refresh
(~2.3 s) runs rarely: once per N updates (50 by default), on entering
NEUTRAL, on a rectangle over half the screen, or on request - the
FULL_REFRESH command or a long MENU press.

## Heartbeat and the privacy fail-safe

Firmware: when no valid frame (of any kind) arrives from the host for
`HB_TIMEOUT` seconds (default 120, configurable), the panel draws a
neutral screen by itself and switches to NEUTRAL mode. Host: sends PING
every 30 s while idle. This covers screen lock, host reboots, an
unplugged cable, a hung host process. A successfully drawn FRAME_FULL or
FRAME_RECT switches the panel to DASHBOARD from any mode, including
NEUTRAL: new content replaces the neutral screen.

## Text debug mode

Lines travel over the same port, terminated by `\n`:

- to the panel: `ping`, `mode <0|1|2>`, `refresh` - the same actions
  without binary encoding (`mode 2` draws the neutral screen at once);
- from the panel: firmware debug lines (`[epd]`, `[key]`, ... prefixes);
  a binary PONG frame answers both the binary PING and the text `ping`.

ASCII lines never collide with the binary magic, so there is no
ambiguity.

## Sizes and timings (reference)

A full frame over UART at 460800 takes about 0.6 s. A smooth update runs
~2.35 s end to end (reset + temperature ~50 ms, compose ~90 ms, RAM
upload ~540 ms, wave ~1660 ms); a full flashing refresh replaces the
wave with a ~2260 ms one. While a wave runs the firmware does not
service the UART, and the 8 KB RX ring covers only ~178 ms of traffic,
so until FRAME_ACK pacing is negotiated through `features` the host must
not stream frames back to back: wait for the previous update to land
(a PING round trip is a safe spacer) or bytes are lost to "bad CRC".
