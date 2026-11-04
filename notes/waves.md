# How screen updates work on this panel

Everything on this page was measured on a real CrowPanel 5.79,
photo-controlled frame by frame. It is kept here so nobody has to
re-derive it.

## Background in one paragraph

The SSD1683 drives the glass with "waves": timed voltage sequences
described by lookup tables (LUTs). A factory LUT set is baked into
the chip ("OTP"); loading a custom one through register 0x32 is
possible but out of scope here. Command `0x22` starts a wave, and its
argument is not a wave number but a bitmask of stage bits:

```
0x80 clock | 0x40 analog | 0x20 load temperature | 0x10 load LUT
| 0x08 BW mode | 0x04 display | 0x02+0x01 disable
```

The visible result depends on the stage bits, the loaded LUT and the
color mode.

## Measured update sequences (factory LUTs)

| 0x22 arg | what it does | time |
|----------|--------------|------|
| 0xF7 | Full update. Flashes (8 inversions in 2.3 s), drives both directions, clears ghosting. | 2261 ms |
| 0xC7 | Smooth update. No flashing, drives both directions; black comes out as patchy dark gray and ghosting builds up. | 1658 ms |
| 0xFF | Fast, but drives pixels only toward white. | 422 ms |
| 0xDC | Fast partial-window mode, but also drives only toward white. | 318 ms |

## Why there is no fast partial refresh

0xFF and 0xDC can push pixels toward white but cannot set or hold
black. Any update that touches black content needs a full-panel wave,
so this firmware redraws the whole screen on every update and uses
0xC7 for regular frames.

## Trap 1: a cold-started panel fills solid black

Without a temperature measurement cycle before the first full update,
the chip latches an invalid temperature, corrupts its waveform and
fills the screen solid black. Run temp_init on every power-on, before
the first full update:

```
0x18 0x80        select the internal temperature sensor
0x22 0xB1, 0x20  load the temperature
0x1A 0x64 0x00   write it to the waveform temperature bucket
0x22 0x91, 0x20
0x3C 0x03        border waveform
```

After temp_init, clearing the screen gives white at any 0x1A value.
The 0x1A value itself (temperature bucket) showed no measurable
effect on timing or density (swept 0x00..0x64).

## Trap 2: the smooth update only stays smooth right after reset + temp

0xC7 without a hardware reset and a fresh temperature cycle degrades
into a flashing full-like drive: 2.26 s BUSY with whole-screen
inversions, caught on a photo series. The factory firmware runs
reset+temp before every menu redraw; this firmware does the same in
`prepWorkUpdate()` before every 0xC7.

Measured cost of one smooth update, end to end: reset+temp ~50 ms,
frame compose ~90 ms, RAM upload ~540 ms, wave 1660 ms - about
2.35 s in total.
