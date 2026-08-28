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
- **Hardware: rev 2.0 — NOT YET FABRICATED.** A 10-piece pilot run is on order.
- **Firmware: ⚠️ NOT WRITTEN FOR THIS BOARD.**

> ### ⚠️ The firmware in this repo will NOT drive this board
> `firmware/src/noknok_leds.c` is inherited from the 8× RGB module and is wrong here on
> three counts:
> 1. `#define LED_COUNT 8` — this board has **16**
> 2. `WS_BUF_LEN (LED_COUNT * 24 …)` — 24 bits/LED (GRB). RGBW needs **32 bits/LED (GRBW)**
> 3. The wire protocol packs **3 bytes per LED** (`cmd_buf[i*3+…]`) — needs a 4th channel
>
> Flashed as-is it sends 192 bits where 512 are needed: the first LED or two show garbage
> and LEDs 3–16 stay dark. It will look like a hardware fault.
>
> The source is kept deliberately as the **starting point for the rewrite**, not as a
> working binary. The 4-byte change also alters the wire format the Conductor uses, so it
> needs coordinating with `noknok_usb.py` (Sue) — preferably by reporting channel count in
> the vendor command response rather than hardcoding it per module type.
>
> **Also note:** the driver is a hand-rolled TIM2_CH4 + DMA polling bitstream tuned for
> WS2812b timing. SK6812 timing differs slightly. See the LED Button lesson in `Sam.md` —
> a marginal waveform perfectly mimics a hardware/component fault. Bench-validate the
> waveform before concluding anything about the LEDs themselves.

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
