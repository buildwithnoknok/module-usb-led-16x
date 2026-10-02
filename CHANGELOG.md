# Changelog - noknok LEDs 16x module firmware

Newest first. One entry per version bump, written in the same commit as the bump. Format:
`## <version> - <date>` then `- DEV-xx - what changed and why. Breaking: yes/no.`
Convention: Confluence "Development Conventions" (SD space, page 120160257).

## 2.2.1 - 2026-10-02

- DEV-28 - Fix USB-C source detection: the CC pins are read through the board's 3.3 k / 1.8 k Rd divider and are now scaled back to the line voltage. Before, a 3 A USB-C source was treated as standard USB (LED budget stuck at 450 mA); now budget 1000 mA, measured 0.63 A / 3.18 W on a 5 V / 3 A charger. The PA3/PB0 LED-rail rule is now a hard check inside the LED driver (reads PB0's output register). Breaking: no.

## Baseline - 2026-10-02

- Changelog starts here. Current: firmware 2.2.0 (FW_VERSION in firmware/src/noknok_leds.c). Earlier release notes: firmware/readme.md. Earlier history: git log and the README.
