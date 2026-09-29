# Firmware — noknok LEDs 16x

Firmware for the noknok LEDs 16x module (CH32V203G6U6 — USB, 16× SK6812 **RGBW**, rev 2.0
round board). **Not** for the 8× RGB board — that one lives in `module-usb-led`.

- Source: `src/` (built with [ch32fun](https://github.com/cnlohr/ch32fun); `Makefile` uses `extralibs/usbd.c`)
- Binaries: `bin/noknok_leds.bin` and `bin/noknok_leds.hex` — current release (app only,
  linked at `0x2000`, runs under the noknok USB bootloader)
- USB identity: VID `0x1209` / PID `0x4E4E` (app) / PID `0x4E42` (bootloader). Appears as a CDC serial device ("noknok LEDs"). Each unit has a **unique serial number** derived from its chip UID.

Firmware is licensed MIT (see `../LICENSE`). Toolchain and protocol follow the
[noknok Ecosystem guidelines](https://github.com/buildwithnoknok/Ecosystem).

## LED layout
LED index **0–11 = outer ring, clockwise**; **12–15 = the 4 inner LEDs, anti-clockwise**
(looking at the LED side). Each LED has 4 dies: R, G, B and a dedicated white (neutral
white phosphor, one half of the lens). Wire order to the LED is G R B W.

## Flashing a blank board (SWD, one pass)
The rev 2.0 board has the 4-pad noknok flash footprint (GND / SWDIO / 3V3 / SWCLK — SWCLK is
**required** on the CH32V203). Build one combined image (USB bootloader + app + validity
metadata) with `module-USB-bootloader/tools/combine_image.ps1 -AppBin bin/noknok_leds.bin`,
then write it at `0x08000000` with a WCH-LinkE — see `../tools/bench/flash_leds16.sh`
(writes, reads back, compares, reboots).
> **Do not use `minichlink -t`** (target power off) on this chip — minichlink segfaults on the
> write. Power the board from its own USB-C and leave the LinkE's 3V3 pad unpowered/unused.

After that, app updates go over USB through the noknok bootloader (OTA), no programmer.

A brief **dim warm-white boot flash (~0.3 s, white die only)** means the firmware is running;
a **"noknok LEDs" serial port** then appears.

## Controlling the module
- From the Pi/PC: bench scripts in [`../tools/bench/`](../tools/bench) (`test_leds16.py`,
  `step_leds16.py`, `stress_leds16.py`).
- From a Raspberry Pi Pico (USB host): the `NoknokLEDs` class in brain-Pico
  `software/noknok_usb.py` — **not yet updated for 16 LEDs / white** (needs `LED_COUNT = 16`
  and the `0x1x` white commands).

**Command protocol** (raw bytes, host → module). RGB commands set white = 0.

| Bytes | Action |
|-------|--------|
| `0x00` | all LEDs off (LED power rail is switched off) |
| `0x01 R G B` | set all 16 LEDs |
| `0x11 R G B W` | set all 16 LEDs, with white |
| `0x02 i R G B` | set LED `i` (0–15) |
| `0x12 i R G B W` | set LED `i` (0–15), with white |
| `0x03 B` | global brightness 0–255 |
| `0x04 [48 bytes]` | set all 16 LEDs: 16 × (R G B) |
| `0x14 [64 bytes]` | set all 16 LEDs: 16 × (R G B W) |
| `0x05` | explicit show |
| `0x10 i R G B BR dLo dHi` | **SET_LED** — LED `i` (`0xFF` = all), colour, brightness, duration ms little-endian (0 = hold) |
| `0x18 i R G B W BR dLo dHi` | **SET_LED** with white |
| `0x20 preset speed R G B` | **PLAY_PRESET** 1–6 (rainbow / breathe / theatre-chase / colour-wipe / twinkle / **sundown**); `speed` = ms per step (0 = default) — **EXCEPT preset 6 (sundown), where `speed` means MINUTES instead (0 = default 30)**; `R G B` = base colour |
| `0x21 preset speed R G B W` | **PLAY_PRESET** with white (e.g. sundown in white) |
| `0xF0` | identity query → module replies `0x4E 0x4E 0x04` |
| `0xB1` | version query → replies `[PROTOCOL, MAJOR, MINOR, PATCH]` |
| `0xB0` | enter the USB bootloader (OTA) — no reply, resets |

An incomplete command is dropped after 50 ms. USB receive is lossless (flow-controlled), so a
host may send commands back-to-back without pauses.

## Power (measured 29 Sep 2026, rev 2.0, KWS-X1 inline on USB-C)
| State | Draw |
|---|---|
| All off (LED rail switched off) | 0.03 W (~6 mA) |
| White die only, 16 × 100 % | 0.95 W |
| Everything requested at 100 % (current cap active) | 1.78 W (~0.35 A) |

The firmware caps the estimated LED current at 450 mA (model: 12 mA per channel at full,
measured). Uncapped, all 64 channels would draw ~0.7 A. The cap becomes CC-pin / VBUS aware
later.

**Known hardware issue (rev 2.0):** at high brightness a faint "rustle" is audible — the
SK6812's ~1.1 kHz internal PWM makes the LED supply current pulse, and a ceramic capacitor
(or the board) vibrates. Touching the PCB damps it. Hardware follow-up pending.

---

## Firmware Change Record

### v2.0.2 — first firmware for the 16x RGBW board (current) — bench-validated 2026-09-29
Bench-validated on a rev 2.0 pilot board (all 16 LEDs, every channel, by eye; KWS-X1 power
readings above). v2.0.0 / 2.0.1 were bench-only steps on the way, never released.
- **16 LEDs, RGBW.** 32 bit per LED (G R B W), new white commands `0x11/0x12/0x14/0x18/0x21`;
  the RGB commands keep working (white = 0), except `0x04` which now carries 48 bytes.
- **Driver: TIM2_CH4 PWM + DMA only — no bit-banging.** SK6812 timing: 1.25 µs bit slot,
  T0H 0.31 µs, T1H 0.60 µs (spec centre; the 8× value of 0.79 µs was outside the SK6812
  spec), reset 100 µs. Output-compare **preload** on, so the DMA'd pulse width only applies
  at the next bit boundary — no pulse is ever clipped mid-bit. DMA priority very high.
- **LED rail sequencing (PB0).** PA3 (data) is driven low from the first instruction, the
  rail switches on only before a non-black frame (1 ms settle) and off when the frame is all
  black. Never drives data into unpowered LEDs (the level shifter U3 is always enabled and
  would back-feed the rail). Standby with LEDs off: 0.03 W.
- **Current cap** 450 mA estimated LED current, 12 mA/channel model (bench-calibrated).
- **Lossless USB receive (2.0.1 fix).** ch32fun `usbd.c`'s printf receive path has a 64-byte
  buffer that throws away the OLDEST bytes if a second packet arrives before the main loop
  polls. A 65-byte `0x14` frame lost its command byte; the colour bytes were parsed as
  commands (`0x00` = all off → dark ring). Now: `FUNCONF_USE_USBPRINTF 0`, own 256-byte ring
  in the USB IRQ, and the IRQ is masked when less than one packet of room is left, so the
  endpoint NAKs and the host retries (true flow control). Proven with 300 back-to-back
  `0x14` frames (`tools/bench/stress_leds16.py`): 2.0.0 went dark, 2.0.2 passes.
  ⚠ The 8× `module-usb-led` firmware still uses the old receive path.
- 50 ms parser timeout drops an incomplete command (e.g. an old host sending 24-byte `0x04`).
- Flash 7108 B (29 % of the 24 KB app region), RAM 2040 B.

### v1.8.1 — sundown preset (8× board) — released 2026-07-02
- **New preset 6 = SUNDOWN.** `0x20 6 minutes R G B` plays a **one-shot** fade from full
  brightness to off in the caller-supplied colour, over `minutes` minutes (not the usual
  `speed`-as-ms/step — see below). Unlike presets 1–5, which loop forever until the next
  command, SUNDOWN runs once and stops itself (LEDs off) at the end instead of repeating.
  Intended for a bedtime/wind-down light (e.g. blue, 30 min).
- **Easing is CONCAVE (quadratic ease-out), not linear:** brightness follows
  `(1 - elapsed/total)²`, so it dims fast at the start and slows to a crawl near the end
  (e.g. at the halfway point in time, brightness is at 25%, not 50%). Computed in Q10
  fixed-point integer math — no FPU on this MCU, same approach as the rest of the firmware.
- **`speed` field is repurposed for this preset only:** every other preset's `speed` byte
  means "ms per animation step", which can't encode a useful multi-minute total duration in
  one byte. For preset 6 only, `speed` means **minutes** (1–255; 0 defaults to 30). This is a
  deliberate, documented exception — not a silent redefinition of the field ecosystem-wide.
- Flash usage 6328 B (26% of 24 KB app region), RAM 1060 B. No other behavioural changes
  from v1.8.0 (see v1.8.0 below for the OTA/bootloader-hosted architecture change).

### v1.8.0 — OTA-capable, bootloader-hosted — released 2026-06-23
- App relinked at `0x2000` (`app.ld`) to run under the noknok USB bootloader
  (`module-USB-bootloader`), with the top 16 B of RAM reserved as the handoff cell.
- `0xB0` ENTER_BOOTLOADER: writes the handoff magic + resets; the bootloader catches it and
  stays in USB flashing mode. Updates now flash over USB via `usb_flash.ps1` / the Pico's
  `UsbModuleFlasher` — no BOOT0 jumper needed after the bootloader's one-time flash.

### v1.6 — unique serial + rich LED control & presets — released 2026-06-18
- **Unique USB serial number.** `iSerialNumber` is now built at boot from the CH32V203's 96-bit hardware chip UID (ESIG at `0x1FFFF7E8`) as 24 hex characters, so every physical module enumerates with a **distinct serial**. This is the USB counterpart of the I2C hardware UID — it lets the Conductor tell identical modules apart and map them to roles. (Previously all modules shared the hardcoded serial `"007"`.)
- **`0x10` SET_LED — full per-LED control in one command:** `0x10 i R G B brightness durLo durHi`. `i` = LED index (`0xFF` = all); `duration` is 16-bit ms (little-endian; `0` = hold indefinitely, otherwise the LED(s) auto-off after the duration). Non-blocking.
- **`0x20` PLAY_PRESET — 5 built-in non-blocking animations** that run on the module (fire-and-forget, mirroring the buzzer's preset tunes): `1` rainbow rotate, `2` breathe, `3` theatre chase, `4` colour wipe, `5` twinkle. `0x20 preset speed R G B` — `speed` = ms per animation step (`0` = default 40 ms); `R G B` = base colour (ignored by rainbow). Any immediate command (`0x00`–`0x05`, `0x10`) cancels a running animation.
- Added a **TIM3 free-running 1 ms timebase** for durations and animation stepping.
- Flash usage 6052 B (18% of 32 KB), RAM 1056 B.

### v1.5 — adds firmware version query — released 2026-06-17
- **`0xB1` GET_VERSION command** (noknok ecosystem standard, command range 0xB0–0xBF). The module replies with 4 bytes: `[PROTOCOL_VERSION, FW_MAJOR, FW_MINOR, FW_PATCH]` = `0x01, 1, 5, 0`. Lets the host (Pico or PC) read which firmware a module is running — needed by the OTA update flow. Matches the I2C modules' `0xB1` convention.
- No other behavioural changes from v1.4.

### v1.4 — first working release — released 2026-06-04
- **USB stack switched from USBFS (fsusb) to USBD (FSDEV).** On this board the USBFS controller's D+ pull-up is not host-visible, so it never enumerated; the USBD/FSDEV controller (pull-up via `EXTEN_USBD_PU_EN`, the same path the WCH bootloader uses) enumerates reliably as a USB CDC device.
- **Manual clock bring-up: HSE 24 MHz × 2 = 48 MHz** (crystal-accurate 48 MHz USB clock). ch32fun's automatic CH32V203 clock setup does not work for this part (no flash wait-states above 24 MHz; it disables HSI mid-switch), so the firmware boots on the internal HSI and switches to the crystal + PLL itself.
- **Behaviour:** dim-white boot flash on power-up, then enumerates as "noknok LEDs" (VID `0x1209` / PID `0x4E4E`). LED commands are sent as raw bytes over the CDC data endpoint.
- Driveable from a PC (`../tools/`) or from a Pico acting as USB host (`usb.core`).

### v1.0 – v1.3 — bring-up (superseded, non-functional)
- Early builds used the USBFS (fsusb) stack and ch32fun's default 144 MHz clock. They did not run / did not enumerate due to the CH32V203 clock issues described under v1.4. Superseded.

### Abandoned experiment — full CDC line-coding (not released, no version assigned)
- An attempt (between v1.4 and v1.5) to add full CDC line-coding so standard host serial APIs (e.g. .NET `SerialPort.Open`) could open the port. Caused Windows "Code 10 — device cannot start"; root cause not isolated, so it was dropped. (PC tools instead open the port via a raw handle / pyserial, which works.)

## Known limitations
- The minimal CDC implementation does not fully answer line-coding requests, so .NET `SerialPort.Open()` is fussy. Use the provided PowerShell/Python tools (raw handle / pyserial), or `usb.core` raw bulk on the Pico.
- USB VID/PID are pid.codes **prototype** values (`0x1209` / `0x4E4E`) — assign production IDs before release.
