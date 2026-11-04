# crowpanel-display-firmware

Firmware that turns the [CrowPanel 5.79"] e-paper HMI into an external
display for a computer. The computer renders black-and-white pictures
and streams them over USB; the panel draws them and reports button and
encoder events back. What the panel shows is decided entirely by the
host: send a new picture to change it, no reflashing.

A privacy fail-safe is built in: if the host stops sending anything
for `HB_TIMEOUT` seconds (default 120), the panel replaces the content
with a neutral screen by itself. This covers an unplugged cable, a
sleeping host, a crashed one.

## What works (verified on real hardware)

- The full chain: receive a picture over USB, show it, report buttons
  and encoder turns back; heartbeat, refresh policy, the fail-safe.
- The display driver. This panel is two SSD1683 controllers wired as
  one long screen, which no existing library (GxEPD2, Adafruit_EPD)
  supports - so the driver here is our own. Regular updates are
  smooth, without flashing, and take about 1.7 s; a full refresh
  takes about 2.3 s and flashes.

## Hardware

- CrowPanel 5.79: 272x792 e-paper, two cascaded SSD1683 controllers,
  ESP32-S3-WROOM-1-N8R8 (8 MB flash, 8 MB PSRAM).
- USB-UART bridge CH340: one port (`/dev/ttyUSB0`) carries flashing,
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

The wire format is a small binary frame: `A7 1C` + cmd + len (u16 LE)
+ payload + CRC32. A complete client in a few lines of Python:

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
the full contract: rectangles, modes, key events, heartbeat.

## Text debug mode

In `pio device monitor` (or any terminal at 460800) plain text lines
work without any binary encoding:

```
ping          -> pong <uptime_ms> <mode>
mode 2        -> switch mode (0 splash / 1 dashboard / 2 neutral)
refresh       -> draw the next frame with a full refresh
```

Firmware debug lines (`[epd] ...`, `[rx] ...`, `[scan] ...`) travel
over the same port.

## The display driver

`src/ssd1683_epd.{h,cpp}` drives the two-controller cascade. Policy:
smooth updates carry regular frames; a full flashing refresh runs on
mode changes, every 50 frames, on rectangles over half the screen and
on request (the `FULL_REFRESH` command or a long MENU press). Frame
composition costs ~100 ms, the RAM upload ~540 ms. How the update
sequences work - including two traps that will bite anyone reusing
this chip - is written up in [notes/waves.md](notes/waves.md).

## Panel quirks (measured, kept so nobody re-derives them)

- Inverted pixels: the controllers use 0 = black, 1 = white. The
  protocol carries 1 = black; the driver inverts.
- Two chips per line: a screen line is 100 bytes - bytes 0..49 go to
  the first controller (X 0..395), bytes 50..99 to the second
  (X 396..791), with an 8-column gap in the addressing.
- Every update redraws the whole screen. The fast partial modes this
  chip offers (0xFF, 0xDC) can only push pixels toward white - they
  cannot draw black. Details in [notes/waves.md](notes/waves.md).
- Cold start: a temperature measurement must run before the first
  full update, otherwise the screen fills solid black.
- UART: 921600 baud drops bytes in long streams on the CH340 bridge;
  use 460800.

## License

MIT, see [LICENSE](LICENSE). Third-party projects studied while
writing the driver are listed in [NOTICE](NOTICE.md).

[CrowPanel 5.79"]: https://www.elecrow.com/crowpanel-5-79-e-paper-hmi-272x792-display.html
[PlatformIO]: https://platformio.org
