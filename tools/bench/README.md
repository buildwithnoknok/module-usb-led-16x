<!-- SPDX-FileCopyrightText: 2026 noknok (Christopher Houben) -->
<!-- SPDX-License-Identifier: MIT -->
# tools/bench - LEDs 16x bench helpers (bench host with the module on its own USB port)

| File | What it does |
|---|---|
| `flash_leds16.sh` | SWD flash of a blank CH32V203 (4-pad footprint): write, read back, compare, reboot |
| `test_leds16.py`, `step_leds16.py` | drive the module over its CDC port without a Pico (patterns / brightness steps) |
| `status_leds16.py` | governor status (USB-C CC, VBUS, temperature); `log SECS EVERY FILE` writes a CSV |
| `stress_leds16.py` | hundreds of back-to-back frames from a fast host; the ring must show the final frame |
| `heat_run.sh [CSV]` / `heat_stop.sh` | full power + 20 min governor log in the background (housing heat, DEV-55) / stop and switch off (DEV-77: were only on a bench host) |
