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
needed. `len` is the payload length, 0..65535 on the wire; 26928 (FRAME_FULL)
is the largest payload the protocol ever carries, and the receive
buffer is exactly that size. A frame claiming more is rejected right at
the length field with ERROR BAD_LENGTH: a length beyond the largest
command means the peers disagree on the frame size (a configuration
mismatch), and that must be visible, not silent. The parser does not
read the payload: it skips exactly len+4 bytes (payload and its CRC),
so a magic-like sequence inside cannot spawn a phantom frame that
swallows the next real one. A well-sized payload that does not match
the command is rejected with ERROR BAD_LENGTH (below); a mid-frame
break longer than 500 ms resynchronizes the parser back to SYNC.

## Sequence numbers

seq is present in every binary frame of both sides. The host counts its
outgoing frames from 1, skipping 0 on an 8-bit overflow. A response
(PONG, READY, later ERROR/FRAME_ACK/CONFIG_VALUE) echoes the seq of the
request it answers. Autonomous panel frames (the boot READY, KEY_EVENT)
use the panel-side counter. Text-mode lines are not frames and carry no
seq at all.

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
SHADOW_CRC (shadow_crc in PONG, see Shadow consistency), 0x0004 TIME
(clock on the neutral screen), 0x0008 RLE (reserved, not activated
until it proves itself on real frames). The bits gate optional behavior
within the same major version: the panel sets the bits for the features
it implements and a host checks the bit before relying on a feature;
bits a host does not know are ignored, so the bitmap is
forward-compatible in both directions (an old host keeps working with a
new panel and vice versa, gated to the common bits).
`shadow_valid` is 0 until the first FRAME_FULL after startup (a RECT
before that has no base to apply to), 1 after.

## Host -> panel commands

| cmd  | name          | payload                                                        |
|------|---------------|----------------------------------------------------------------|
| 0x01 | PING          | empty; answered with PONG                                      |
| 0x10 | FRAME_FULL    | 26928-byte bitmap, full frame                                  |
| 0x11 | FRAME_RECT    | x,y,w,h (u16 LE) + rect bitmap, partial refresh                |
| 0x12 | CLEAR         | 1 byte: 0 white, 1 black                                       |
| 0x13 | SET_MODE      | 1 byte: 0 SPLASH, 1 DASHBOARD, 2 NEUTRAL (draws the neutral screen at once) |
| 0x14 | FULL_REFRESH  | empty; draw the next frame with a full refresh                 |
| 0x15 | SET_CONFIG    | key=value tuples (below); answered with CONFIG_VALUE           |
| 0x16 | HELLO         | protocol version (u8); answered with READY                     |
| 0x17 | TIME          | hour, minute (u8 each); stores the clock, no ACK; in NEUTRAL redraws when the value changes |

SET_CONFIG: repeating `key_len(1) key value_len(2 LE) value` tuples.
The only known key so far is `hb_timeout`: the value is ASCII-decimal
seconds for the heartbeat fail-safe timeout. Rules: 0, non-numbers and
u32 overflows are rejected with ERROR BAD_VALUE and leave the previous
value unchanged; 1..29 clamp to 30; above 3600 clamps to 3600; anything
else applies as-is. A successful tuple is answered with CONFIG_VALUE
carrying the applied u32 (observability: the host sees the clamp).
Unknown keys are rejected with ERROR BAD_VALUE. Tuples apply
sequentially: a rejected tuple leaves the values already applied in
place (each successful tuple still got its own CONFIG_VALUE). The
configuration is volatile - a panel reboot resets it to the defaults,
and the host is expected to re-apply its desired configuration when it
sees a boot READY mid-session. The host verifies the CONFIG_VALUE echo
against its own clamp computation; a disagreement is a loud error, not
a silent drift.

## Panel -> host commands

| cmd  | name       | payload                                                            |
|------|------------|--------------------------------------------------------------------|
| 0x02 | PONG       | uptime_ms (u32 LE) + mode (u8) + last_drawn_seq (u8) + shadow_crc (u32 LE); last_drawn_seq updates BEFORE FRAME_ACK is sent - this ordering is what makes the lost-ACK fallback sound. It stays 0 until the first frame is drawn (safe: the host never sends seq 0), and the lost-ACK recovery compares it by equality - wrap-safe across 255 -> 0, no side ever compares seqs for order |
| 0x81 | READY      | protocol_version (u8) + features (u16 LE) + shadow_valid (u8); at startup and on HELLO |
| 0x82 | ERROR      | seq of the rejected frame (u8) + code (u8)                         |
| 0x83 | FRAME_ACK  | seq of the drawn frame (u8; the same byte as the payload); sent after a FRAME_FULL/FRAME_RECT/CLEAR wave completes (SET_MODE draws too but is state-only - no ACK) |
| 0x84 | CONFIG_VALUE | applied value (u32 LE); answers a successful SET_CONFIG tuple |
| 0x80 | KEY_EVENT  | key (u8) + event (u8) + value (i16 LE)                             |

The panel answers a binary frame it cannot execute with exactly one
ERROR. A response frame (PONG, READY, ERROR) always carries the seq of
the request it answers - in the header and in the payload; only
autonomous frames (boot READY, KEY_EVENT) use the panel-side counter,
so (cmd, seq) uniquely identifies a response. Frames failing the magic
or the CRC32 are dropped silently at the parse level and never get an
ERROR: with a corrupted body the seq cannot be trusted. The receiver
resynchronizes on the next magic. ERROR frames are written to the TX
buffer synchronously: a host that stops reading stalls the panel, the
watchdog resets it and the panel reboots into the startup state.

| code | name            | when                                                          |
|------|-----------------|---------------------------------------------------------------|
| 0    | -               | reserved, never sent                                          |
| 1    | UNKNOWN_CMD     | cmd is not a known command                                    |
| 2    | BAD_LENGTH      | payload length does not match the command                     |
| 3    | OUT_OF_BOUNDS   | FRAME_RECT outside the 272x792 screen                         |
| 4    | BAD_VALUE       | SET_MODE outside 0..2                                         |
| 5    | SHADOW_INVALID  | FRAME_RECT before the first FRAME_FULL (no shadow base)       |

## Shadow consistency

`shadow_crc` is the CRC32 (the same IEEE table as the frames) of the
panel's full shadow buffer - the last drawn frame with every rect
applied, 26928 bytes in the protocol orientation. A host that tracks
what it sent keeps its own copy of the shadow: a full frame replaces
it, a rect is blitted into it. On every PONG the host compares its copy
against `shadow_crc`; a mismatch (a lost write, a reboot, a bug) means
the copies diverged, and the host's next send is a FRAME_FULL, not a
RECT. READY reports `shadow_valid = 0` after startup for the same
reason: the first frame after a panel reboot is always a full one. A
RECT arriving before that gets ERROR SHADOW_INVALID. Once the copies
match again (`shadow_crc` agrees), the host goes back to rects.

A rect outside the 272x792 screen is rejected with ERROR OUT_OF_BOUNDS,
never clipped: both sides validate the geometry identically before any
pixel moves, so a clipping divergence cannot arise by construction.
CLEAR fills the shadow with the solid color on the panel side; a host
that sends CLEAR must fill its own shadow with the same color - a raw
send would diverge the shadows, and the mismatch recovery above would
redraw the pre-clear image over the cleared screen. The check runs on
every PONG, so with a host heartbeating every N seconds the worst-case
stale-content window after a silent divergence is one heartbeat period.
When the heartbeat fail-safe draws the neutral screen, the panel shadow
becomes all-white and disagrees with the host shadow by design: the
next ping raises the mismatch flag and the host redraws its content -
while the host is alive it is the source of truth for the panel.
A host that never draws (heartbeat only) keeps an empty shadow: while
the panel shows the built-in neutral screen the CRC intentionally
disagrees, so the host skips the check for a neutral PONG mode; the
first PONG after leaving NEUTRAL raises the flag and the next send is
a full frame.
The firmware updates its shadow together with the draw command, before
the wave runs; a power loss mid-wave reboots the panel, and startup
resets `shadow_valid` to 0 - a wrong CRC claim cannot persist silently.

Text-mode garbage and unrecognized text lines get no ERROR: the text
mode has no seq and its stray lines are legitimate resynchronization.
Text commands report a rejected argument as a debug line (`[rx] ...`)
on the same port - ERROR frames are binary-only.

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

Firmware: when no accepted frame arrives from the host for
`hb_timeout` seconds (default 120; SET_CONFIG clamps the key to
30..3600), the panel draws a neutral screen by itself and switches to
NEUTRAL mode. Only frames the
panel accepts reset the timer (a drawn frame, an applied SET_CONFIG, a
PING - all of them; narrowing hb_timeout through SET_CONFIG can
therefore never fire the fail-safe on the spot); rejected frames (the
ERROR answers) do not - a host that keeps sending garbage must not hold
the panel off its privacy fail-safe. Host: sends PING every 30 s while idle. This covers
screen lock, host reboots, an unplugged cable, a hung host process. A
successfully drawn FRAME_FULL switches the panel to
DASHBOARD from any mode, including NEUTRAL: new content replaces the
neutral screen. A FRAME_RECT composes over the current screen content:
coming out of the built-in neutral screen with a rect lands the delta
on top of the clock digits, so a host leaving the panel-side NEUTRAL
must send a FRAME_FULL first - or accept the composite until the next
full frame. Freshly connected hosts get this automatically (a
reconnect starts with a forced full frame).
The neutral screen itself is a white field plus a large HH:MM centered,
once the host has set the clock (TIME 0x17). The panel has no RTC: at
a heartbeat timeout the clock freezes at the last accepted value, and
the timeout neutral screen marks the staleness by hiding the colon
(HH MM); a TIME received while NEUTRAL redraws the full HH:MM, so the
clock ticks on the neutral screen as long as the host keeps sending.
While the host is alive the staleness is bounded by the TIME period
(a lost TIME is re-sent on the next minute tick); a dead host leaves
the stale time on the glass indefinitely. The clock carries the
host's local wall time: NTP steps and DST changes propagate with the
next minute tick, the panel never validates timezones.
Both TIME-in-NEUTRAL and SET_MODE(NEUTRAL) start a wave without an ACK
to pace the host (they are state-only commands). A frame the host
sends within ~3.5 s of such a command can be lost to the RX overflow
(the 8 KB ring covers ~178 ms of traffic and the wave blocks the
loop); a lost DRAW is detected via the missing FRAME_ACK and resent -
that failure self-heals at the cost of one timeout. The state-only
commands themselves are best-effort: the panel cannot detect their
loss, so the host re-sends TIME on its period and can verify a mode
change through the mode field of PONG. A host that draws right after
setting the clock should wait for the wave (~3.5 s) or accept the
resend. The same window deafens the button and encoder polls (they run
in the same loop): short presses inside a wave are lost.

## Text debug mode

Lines travel over the same port, terminated by `\n`:

- to the panel: `ping` - answered with one text line, exactly:
  `[pong] uptime=<ms> ms mode=<0|1|2> shadow_crc=0x<8 lowercase hex>`;
  `mode <0|1|2>`, `refresh` - the same actions without binary encoding
  (`mode 2` draws the neutral screen at once); `time HH:MM` - sets the
  panel clock (same effect as the binary TIME); `ackoff`/`ackon` -
  suppress/re-enable FRAME_ACK (simulates a lost ACK to exercise the
  host fallback: PING with last_drawn_seq);
- from the panel: firmware debug lines (`[epd]`, `[key]`, ... prefixes).

ASCII lines never collide with the binary magic, so there is no
ambiguity.

## Sizes and timings (reference)

A full frame over UART at 460800 takes about 0.6 s. A smooth update runs
~2.35 s end to end (reset + temperature ~50 ms, compose ~90 ms, RAM
upload ~540 ms, wave ~1660 ms); a full flashing refresh replaces the
wave with a ~2260 ms one. While a wave runs the firmware does not
service the UART, and the 8 KB RX ring covers only ~178 ms of traffic -
so the host paces frames: it does not send the next frame until
FRAME_ACK(seq) for the previous one arrives, or, after a timeout, a
PONG shows last_drawn_seq == seq (the wave completed, only the ACK was
lost). A host that ignores this loses bytes to "bad CRC" drops. A frame
sent while the firmware is still booting (before the UART is serviced)
is lost in the FIFO; the missing FRAME_ACK plus a PONG showing an
unchanged last_drawn_seq makes the host resend once - the path
self-heals.
