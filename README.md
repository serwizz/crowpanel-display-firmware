# crowpanel-display-firmware

Firmware for the [CrowPanel 5.79"] e-paper HMI that turns it into a dumb
framebuffer display for a computer. The host renders 1bpp frames and
streams them over USB; the panel draws them on e-ink and sends button
and encoder events back. All content lives on the host - flashing the
panel is never needed to change what it shows.

Privacy fail-safe is built into the firmware: if the host goes silent
for `HB_TIMEOUT` seconds (default 120), the panel clears itself to a
neutral screen. This does not trust the host to behave.

Status: the protocol side is complete and battle-tested on real
hardware (frame parser, buttons, encoder, heartbeat, refresh policy).
The dual-SSD1683 driver (`Ssd1683Epd`) is integrated and verified on
the glass: orientation confirmed with marker frames, the workhorse
update runs smooth (no flashing) at the expected 1.66 s, full
flashing refreshes at 2.26 s.

## Hardware

- CrowPanel 5.79: 272x792 e-paper, two cascaded SSD1683 controllers,
  ESP32-S3-WROOM-1-N8R8 (8MB quad flash, 8MB octal PSRAM).
- USB-UART bridge CH340: one port (`/dev/ttyUSB0`) for flashing,
  frames and logs.
- Buttons: MENU on IO2, EXIT on IO1, encoder A/B on IO6/IO4, CONF on
  IO5; all active LOW with pullups.
- Panel power: GPIO7 must be HIGH before any display init.

## Build and flash

Install [PlatformIO], then:

```sh
pio run                # build
pio run -t upload      # flash (same /dev/ttyUSB0, auto-reset works)
pio device monitor     # serial log and debug commands
```

## Talking to the panel without any host software

The wire format is trivial: `A7 1C` + cmd + len(u16 LE) + payload +
CRC32. A complete client fits in a few lines of Python:

```python
import struct, zlib, serial

ser = serial.Serial("/dev/ttyUSB0", 460800)

def frame(cmd, payload=b""):
    body = bytes([cmd]) + struct.pack("<H", len(payload)) + payload
    ser.write(bytes([0xA7, 0x1C]) + body + struct.pack("<I", zlib.crc32(body)))

frame(0x01)                                    # PING -> "pong <uptime_ms> <mode>"
frame(0x12, b"\x00")                           # CLEAR white
frame(0x10, open("frame.bin", "rb").read())    # full frame, 26928 bytes
```

`frame.bin` is the full-frame bitmap: 1 bit per pixel, MSB first, 1 =
black, 34 bytes per row, 792 rows. See [PROTOCOL.md](PROTOCOL.md) for
the complete contract: partial rectangles, modes, key events, heartbeat.

## Text debug mode

In `pio device monitor` (or any terminal at 460800) plain lines work
without any binary encoding:

```
ping          -> pong <uptime_ms> <mode>
mode 2        -> switch mode (0 splash / 1 dashboard / 2 neutral)
refresh       -> next frame with a full refresh
```

The firmware also prints debug lines (`[epd] ...`, `[key] ...`,
`[rx] ...`, `[scan] ...`) to the same port.

## Display driver

`Ssd1683Epd` (`src/ssd1683_epd.{h,cpp}`) implements the `EpdDriver`
seam for this dual-SSD1683 cascade; GxEPD2 and Adafruit_EPD do not
support it. Refresh policy: `0xC7` workhorse updates carry regular
frames (no flashing, 1.66 s, needs a reset + temperature cycle first -
see `prepWorkUpdate`), `0xF7` full flashing
refreshes run on mode transitions, every 50 frames, for rects over
half the screen and by request (`FULL_REFRESH` / long MENU). The
partial window (`0xDC`) is not used: fast BW waves cannot drive black
on this panel (see "Panel quirks"). Frame composition (protocol
bitmap -> cascade layout) costs ~100 ms, the RAM upload ~540 ms.
Orientation follows the factory coordinate model (protocol Y along
the long axis); flip flags in the constructor default to it. The
measured wave model (0x22 stage bits, 0xF7/0xC7/0xFF/0xDC behavior,
the cold-start and 0xC7 traps) is written up in
[notes/waves.md](notes/waves.md).

Two references were studied for the integration: the official Elecrow
Arduino library for this panel (GoodDisplay-style `EPD_Init`,
`EPD_ShowString`, `Paint_*`) and the ESPBoards
esphome-lvgl-crowpanel-epaper-5.79-4.2 component (partial refresh and
custom LUTs). Neither piece was copied; register sequences are facts
from the SSD1683 datasheet and live experiments.

## Panel quirks (dual SSD1683, learned the hard way)

Measured on real hardware, saved here so nobody re-derives them:

- Black/white inversion: RAM bit 0 = black, 1 = white. The protocol
  carries 1 = black, so the driver must invert.
- Cascade layout: 100 bytes per row; visible X 0..395 goes to the
  master, 396..791 to the slave (with an 8-column gap to the logical
  address); Y scans downward (buffer y=0 lands at the bottom of the
  glass). Confirm X mirroring with a marker during integration.
- OTP waveforms (register 0x22):
  - `0xF7` full 3-color refresh: 2261 ms, flashes (8 inversions in
    2.3 s), drives both ways, cleans ghosting.
  - `0xC7` 3-color display without LUT reload: 1658 ms, no flashing,
    one smooth transition, drives both ways, but black renders as
    patchy dark gray and ghosting accumulates.
  - `0xFF` (422 ms) and `0xDC` (318 ms) fast BW waves drive only
    toward white on this panel - they cannot set black content. This
    single fact explains every earlier "weak drive" partial-refresh
    failure.
- The big one: without a temperature measurement cycle (command
  `0x18 0x80`) before the first `0xF7`, the controller latches an
  invalid temperature, degenerates the LUT and fills the screen SOLID
  BLACK. Run temp_init on every cold start, before the first full
  update. After temp_init a clear gives white at any 0x1A value.
- Register 0x1A (OTP temperature bucket): no measurable effect on
  timing or density on this panel (swept 0x00..0x64).
- 921600 baud over the CH340 drops bytes in continuous streams (a
  full frame was corrupt 2 times out of 3). Everything runs at 460800;
  only esptool upload keeps 921600, it sends block-by-block.

## License

MIT, see [LICENSE](LICENSE). Studied third-party references are listed
in [NOTICE](NOTICE.md).

[CrowPanel 5.79"]: https://www.elecrow.com/crowpanel-5-79-e-paper-hmi-272x792-display.html
[PlatformIO]: https://platformio.org/
