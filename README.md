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
- **Hardware: rev 2.0 — pilot boards received (Sep 2026).** Known issue: an audible "rustle"
  at high brightness from the 22 µF ceramic capacitors on `+5V` (C29/C36/C37) — fix proposed
  for rev 2.1 (see [`firmware/readme.md`](firmware/readme.md) → Power).
- **Firmware: v2.2.2 (own module type `0x06`) — bench-validated 19 Nov 2026:** all 16 LEDs × 4 channels, glitch-free fast updates (DEV-78), animations,
  LED-rail power switching, lossless USB receive, and a **power governor** (USB-C CC source
  limit, VBUS sag protection, **temperature protection** with the chip's calibrated sensor).
  `0x30 GET_STATUS` reports temperature/power for the app. Updates over USB (OTA).
- **Heat:** full power (1.8 W) in open air settles at ~57–58 °C chip temperature;
  measurements in [`docs/measurements/`](docs/measurements/). Thermal thresholds are
  provisional until measured inside the real housing.
- **Still to do:** Pico driver (`noknok_usb.py`) for 16 LEDs + white + `GET_STATUS`, and the
  read-only temperature field in the app (Sue); heat test in the real housing; animations aware
  of the 12 + 4 LED layout; port to the two-stage bootloader; rev 2.1 capacitor fix.

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

## Flashing a blank or bricked module (full image)

`firmware/bin/usb_leds_16x_full.bin` is the **full image**: the noknok USB bootloader, this firmware and its valid-app marker,
over the first 32 KB of flash. Use it for a first flash or a recovery, either with a WCH-LinkE on SWD:

```
minichlink -w firmware/bin/usb_leds_16x_full.bin flash -b
```

or with the BOOT0 jumper + WCHISPTool, written at `0x08000000`. After that, updates go over USB with the app image
`firmware/bin/noknok_leds.bin`, which is what `firmware/index.json` points at.

> **Never flash the app image (`noknok_leds.bin`) at `0x08000000`.** It is linked at `0x2000`; written at the flash
> start it wipes the bootloader and the module no longer starts. Flash the full image to recover.

`firmware/full.json` records which app and bootloader the full image was built from (size and CRC32 of each).
Rebuild it after every firmware release with `python tools/mk_full_image.py` (needs a clone of
[module-USB-bootloader](https://github.com/buildwithnoknok/module-USB-bootloader) next to this repo);
`python tools/mk_full_image.py --check` fails if it no longer matches `firmware/index.json`.

## Repository Structure
- `/hardware` — KiCad files, schematics, 3D models
- `/firmware` — source (`src/`), built binaries (`bin/`), and the firmware change record (`readme.md`)
- `/tools` — PC control scripts
