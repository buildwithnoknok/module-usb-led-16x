# noknok LEDs 16x

A USB-controlled **round** ring of 16 RGBW LEDs for the noknok ecosystem. The Raspberry Pi
Pico drives it as a **USB host**; it can also be driven directly from a PC.

> **Sibling module:** the 8-LED RGB version lives in
> [`module-usb-led`](https://github.com/buildwithnoknok/module-usb-led). Both are being kept
> until the 16x boards arrive and the real optical difference (luminosity, colour, power
> draw) can be measured on the bench.

## Features
- **16× RGBW LEDs** (OPSCO `SKC6812RGBW-NW`, LCSC `C5348912`) — four channels per LED
  (R, G, B **and a dedicated white**), not three
- Round board, 96 components, double-sided assembly
- CH32V203G6U6 microcontroller, USB 1.1 (CDC) device
- USB-C power + data; Qwiic/Stemma-QT I2C pass-through
- **Unique per-unit USB serial** derived from the chip UID — so the Conductor can tell
  identical modules apart and map them to roles (the USB counterpart of the I2C UID)

## Status
- **Hardware: rev 2.0 — pilot boards received (Sep 2026).** Known issue: a faint audible
  "rustle" at high brightness (capacitor/board vibration from the LEDs' ~1.1 kHz PWM) —
  hardware follow-up pending.
- **Firmware: v2.0.2 — bench-validated 29 Sep 2026** on a pilot board: all 16 LEDs, all four
  channels, animations, LED-rail power switching, current cap, lossless USB receive. See
  [`firmware/readme.md`](firmware/readme.md) for the command set, LED layout, flashing and
  measured power.
- **Still to do:** Pico driver (`noknok_usb.py`) for 16 LEDs + white; CC-pin / VBUS-aware
  power budget; animations aware of the 12 + 4 LED layout; port to the two-stage bootloader.

## How it's controlled

### From a Raspberry Pi Pico (USB host) — the noknok Conductor
The Pico hosts the module over PIO-USB (D+ → GP16, D− → GP17, behind a **powered** hub
such as the DataHub). The `NoknokLEDs` driver lives in
[`brain-Pico/software/noknok_usb.py`](https://github.com/buildwithnoknok/brain-Pico/tree/main/software)
and is discovered by the Conductor:

```python
from noknok import Conductor

c = Conductor()
c.enumerate_usb()          # brings up the USB host port, finds all LED modules
# (use c.enumerate_all() for a product that mixes I2C + USB modules)

# NOTE: the Pico driver still speaks the 8-LED RGB protocol - 16x/white support pending
leds = c.leds[0]                                  # or c.by_uid("<serial>"), or c.role["lamp"]
leds.set_all(255, 0, 0)                           # all red
leds.set_pixel(3, 0, 255, 0)                      # LED 3 green
leds.set_brightness(128)                          # global brightness
leds.set_led(0xFF, 0, 0, 255, brightness=200, duration_ms=1000)  # all blue 1 s, then off
leds.play_preset(leds.PRESET_RAINBOW, speed=40)  # non-blocking animation on the module
leds.play_preset(leds.PRESET_SUNDOWN, speed=30, r=0, g=0, b=255)  # 30 min fade to off, blue
leds.off()
```

Each module carries a unique serial, so multiple LED modules on one hub are addressed
individually. See **[Ecosystem → enumeration.md](https://github.com/buildwithnoknok/Ecosystem/blob/main/software/enumeration.md)**
(USB Module Discovery & Identity) and **[roles.md](https://github.com/buildwithnoknok/Ecosystem/blob/main/software/roles.md)**
(role assignment — this is an *output* module, assigned by cue-and-confirm).

### From a PC
PowerShell / Python helpers in [`tools/`](tools) (`noknok_leds.ps1`, `noknok_leds_pc.py`)
auto-detect the module and expose the same commands.

## Repository Structure
- `/hardware` — KiCad files, schematics, 3D models
- `/firmware` — source (`src/`), built binaries (`bin/`), and the firmware change record (`readme.md`)
- `/tools` — PC control scripts
