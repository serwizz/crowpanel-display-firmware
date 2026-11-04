# SSD1683 wave model on the CrowPanel 5.79 (dual cascade)

Everything below was measured on a live panel, photo-controlled and
frame-by-frame. Register `0x22` (Display Update Control 2) is not a
"wave selector": it is a bitmask of stage bits (0x80 clock / 0x40
analog / 0x20 load temperature / 0x10 load LUT / 0x08 BW mode /
0x04 display / 0x02+0x01 disable). What the glass does is determined
by the stage sequence, the loaded LUT and the color mode.

## Measured update sequences (factory OTP waveforms)

| 0x22 seq | kind | time | behavior |
|----------|------|------|----------|
| 0xF7 | 3-color full | 2261 ms | Drives both ways, FLASHES (8 inversions in 2.3 s), clears ghosting. Use rarely: startup, mode transitions, periodic cleanup. |
| 0xC7 | 3-color display, no LUT reload | 1658 ms | No flashing, one smooth transition, drives both ways; black renders as patchy dark gray and ghosting accumulates. The workhorse. |
| 0xFF | BW full | 422 ms | Drives only toward WHITE - cannot set black. |
| 0xDC | BW fast partial window | 318 ms | Drives only toward WHITE - cannot set black. |

The last two rows explain every "weak partial refresh" failure on this
panel: fast BW waves cannot drive black at all, so a classic fast
partial refresh is impossible with the factory OTP LUTs. A custom LUT
(register 0x32) might change this; it is deliberately left for a
future experiment.

## The 0xC7 trap: it needs a reset + temperature cycle first

0xC7 only runs smooth (no flashing, ~1.7 s) right after a hardware
reset plus a temperature measurement cycle:

1. `0x18 0x80` - select the internal temperature sensor;
2. `0x22 0xB1`, `0x20` - load the temperature;
3. `0x1A 0x64 0x00`, `0x22 0x91`, `0x20` - write it to the OTP
   waveform temperature bucket;
4. `0x3C 0x03` - border waveform.

Without this, the controller falls back to a flashing full-like drive
(2.26 s BUSY, whole-screen inversions measured frame by frame). The
factory UI does reset+temp before every menu redraw. In this firmware
see `prepWorkUpdate()` in `src/ssd1683_epd.cpp`.

Measured full path of one work update: reset+temp ~50 ms, frame
compose ~90 ms, RAM upload ~540 ms, 0xC7 wave 1660 ms - about 2.35 s
end to end.

## The cold-start trap: temp_init before the first 0xF7

Without a temperature measurement cycle before the first full
refresh, the controller latches an invalid temperature, degenerates
the waveform and fills the screen SOLID BLACK. Always run temp_init
on every cold start, before the first full update. After temp_init a
clear gives white at any 0x1A value.

Register 0x1A (OTP temperature bucket) has no measurable effect on
timing or density on this panel (swept 0x00..0x64).

## Panel geometry facts the waves rely on

- RAM bit polarity is inverted: bit 0 = black, bit 1 = white.
- Cascade layout: a gate line is 100 bytes; visible X 0..395 goes to
  the master controller, 396..791 to the slave (with an 8-column gap
  in the logical address). See the "Panel quirks" section of the
  README.
